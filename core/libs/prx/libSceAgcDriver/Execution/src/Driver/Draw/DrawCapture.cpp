#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

StageMemoCounts& StageMemoCounters() {
    static StageMemoCounts counts;
    return counts;
}

namespace {

// Whether two vertex stage infos give a request the same interface key (the formats and where the
// attributes go); `sameBuffers` says the vertex buffer table, which the stage input validation
// reads, is the same too.
bool SameVertexInterface(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b, bool& sameBuffers) {
    if (a.resourcesNum != b.resourcesNum || a.resourcesNum > a.resources.size() || a.fetchAttribReg != b.fetchAttribReg || a.fetchBufferReg != b.fetchBufferReg || a.fetchEmbedded != b.fetchEmbedded) return false;
    sameBuffers = true;
    for (std::uint32_t i = 0; i < a.resourcesNum; ++i) {
        const auto& x = a.resources[i].fields;
        const auto& y = b.resources[i].fields;
        const auto& from = a.resourcesDst[i];
        const auto& to = b.resourcesDst[i];
        if (x[3] != y[3] || from.registerStart != to.registerStart || from.registersNum != to.registersNum || from.attrId != to.attrId || from.fetchIndex != to.fetchIndex) return false;
        if (x[0] != y[0] || x[1] != y[1] || x[2] != y[2]) sameBuffers = false;
    }
    return true;
}

// The stage's prepared invocation, from the shape's record when this draw gives the stage what the
// record was resolved from. InvocationFor builds the request's interface key and compares it, the
// code and the stage inputs with the snapshot's prepared entries under their lock, for every stage
// of every draw; the draws of a shape differ in none of that but the vertex buffer table. What
// the shape key covers (the pixel stage info, the mesh and tessellation state, the device) is not
// compared again.
ShaderRecompiler::PreparedShaderInvocation ShapeInvocation(ShapeStage& recall, const DrawProgram& program, const ShaderRecompiler::RecompileRequest& request, std::uint64_t pixelFrom) {
    static const std::uint64_t verifyEvery = [] {
        const char* text = std::getenv("APS5_VERIFY_STAGE_MEMO");
        return text != nullptr ? std::max<std::uint64_t>(std::strtoull(text, nullptr, 0), 1u) : 0u;
    }();
    auto& counts = StageMemoCounters();
    const auto* vertex = request.context.vertex ? &*request.context.vertex : nullptr;
    bool same = recall.handle != nullptr && recall.snapshot == program.snapshot && recall.codeOffset == program.codeOffset && recall.stage == request.shader.stage && recall.waveSize == request.context.waveSize && recall.firstUserSgpr == program.firstUserSgpr && recall.userWords == program.userData.size() && recall.pushOffset == request.layout.pushConstantOffsetBytes && recall.pushSize == request.layout.pushConstantSizeBytes && recall.pixelFrom == pixelFrom && (recall.vertex != nullptr) == (vertex != nullptr);
    bool sameBuffers = true;
    if (same && vertex != nullptr) same = SameVertexInterface(*vertex, *recall.vertex, sameBuffers);
    if (same) {
        const bool validated = sameBuffers || vertex->fetchEmbedded;
        if (!validated) *recall.vertex = *vertex;
        auto reused = request;
        reused.shader.code = ShaderRecompiler::GetPreparedCode(*recall.handle);
        auto invocation = ShaderRecompiler::PreparedShaderInvocation::Reuse(reused, recall.handle, validated);
        const auto reuses = counts.reused.fetch_add(1, std::memory_order_relaxed);
        if (!validated) counts.revalidated.fetch_add(1, std::memory_order_relaxed);
        if (verifyEvery == 0u || reuses % verifyEvery != 0u) return invocation;
        static std::atomic<std::uint64_t> compared{0}, differences{0};
        auto resolved = InvocationFor(*program.snapshot, program.codeOffset, request);
        const bool differs = resolved.Handle() != recall.handle;
        const auto count = compared.fetch_add(1, std::memory_order_relaxed) + 1;
        if (differs && differences.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[stage-memo] verify: program 0x%llx (stage %u) resolves to another artifact than its shape's record holds\n", static_cast<unsigned long long>(program.binary.codeAddress), static_cast<unsigned>(request.shader.stage));
        if ((count & (count - 1)) == 0 || count % 100000 == 0) std::fprintf(stderr, "[stage-memo] verify: %llu reused stages resolved again, %llu differences\n", static_cast<unsigned long long>(count), static_cast<unsigned long long>(differences.load(std::memory_order_relaxed)));
        if (!differs) return invocation;
        recall.handle = resolved.Handle();
        recall.memo = ShaderRecompiler::MakeStageMemo();
        return resolved;
    }
    auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
    counts.resolved.fetch_add(1, std::memory_order_relaxed);
    recall.snapshot = program.snapshot;
    recall.codeOffset = program.codeOffset;
    recall.stage = request.shader.stage;
    recall.waveSize = request.context.waveSize;
    recall.firstUserSgpr = program.firstUserSgpr;
    recall.userWords = static_cast<std::uint32_t>(program.userData.size());
    recall.pushOffset = request.layout.pushConstantOffsetBytes;
    recall.pushSize = request.layout.pushConstantSizeBytes;
    recall.pixelFrom = pixelFrom;
    if (vertex == nullptr) recall.vertex.reset();
    else if (recall.vertex == nullptr) recall.vertex = std::make_unique<ShaderRecompiler::ShaderVertexStageInfo>(*vertex);
    else *recall.vertex = *vertex;
    if (recall.handle != invocation.Handle() || recall.memo == nullptr) recall.memo = ShaderRecompiler::MakeStageMemo();
    recall.handle = invocation.Handle();
    return invocation;
}

}

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected, ShapeStages* shape, std::uint64_t pixelFrom) {
    PerformanceTimer timing("Shader.DrawStage");
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory, RegisteredFloatMode(*program.snapshot)},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    auto* const recall = shape != nullptr && i < shape->stages.size() ? &shape->stages[i] : nullptr;
    const auto invocation = recall != nullptr ? ShapeInvocation(*recall, program, request, program.binary.stage == Stage::Fragment ? pixelFrom : 0u) : InvocationFor(*program.snapshot, program.codeOffset, request);
    timing.Mark("prepared_invocation");
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        return shaderMemory.Capture(invocation);
    }();

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    if (drawHit) {
        for (std::size_t j = 0; j < programs.size(); ++j) {
            if (matched[j] != nullptr && !recompiled[j]) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
        }
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    phaseTiming.Phase(DrawRowCapture);

    timing.Mark("capture_resources");
    stageCapture.compiled = invocation.Materialize(*capture, recall != nullptr ? recall->memo.get() : nullptr);
    timing.Mark("materialize");
    // Shared, not copied: a copy allocated every descriptor binding's vectors again for every stage
    // of every draw (after boykopovar/AnyPS5#1202).
    phaseTiming.Phase(DrawRowRecompile);
    return stageCapture.compiled;
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu)) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            for (const auto& region : regions) {
                variant->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
                const auto count = region.bytes.size() / sizeof(std::uint32_t);
                const auto offset = variant->words.size();
                variant->words.resize(offset + count);
                std::memcpy(variant->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
            }
            if (verifyHit && matched[i] != nullptr && (matched[i]->runs != variant->runs || matched[i]->words != variant->words)) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr);
        if (unstable != 0 || mismatches != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
