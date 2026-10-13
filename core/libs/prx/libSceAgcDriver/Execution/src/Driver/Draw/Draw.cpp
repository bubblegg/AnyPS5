#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <x86intrin.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

// Everything a draw builds between its decode and its commit, and what the commit reads. A draw
// made some forty vectors and map nodes for this on the thread that prepares it, and the thread
// that records it freed them with the commit closure (allocating and freeing were a fifth of the
// one thread and a seventh of the other). The objects are pooled instead: a draw takes one, fills
// it over what the last draw left (the vectors keep their storage, the captured pages too), and the
// commit returns it. The commit closure is then two pointers, which a std::function holds without
// allocating. APS5_NO_DRAW_WORK_POOL=1 makes and destroys one per draw.
struct DrawWork {
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos;
    std::vector<std::vector<Graphics::DecodeRead>> decodeReads;
    ShaderMemory shaderMemory{std::span<const ShaderRecompiler::MemoryRegion>{}};
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> results;
    std::vector<Graphics::CompiledShader> stages;
    std::vector<const ShaderRecompiler::RecompileResult*> programResults;
    std::vector<StageCapture> stageCaptures;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions;
    std::vector<std::shared_ptr<DispatchVariant>> fresh;
    std::vector<bool> recompiled;
    std::vector<std::uint32_t> pushOffsets;
    std::vector<std::size_t> resultIndex;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
    // Each stage's part of the draw's resource cache key (see prepareStageKeys): built here, or
    // shared with the shape's replay record.
    std::vector<std::vector<std::uint32_t>> stageKeys;
    std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> stageKeyRefs;
    // What only the commit reads.
    std::shared_ptr<VulkanDevice> localDevice;
    std::shared_ptr<const DrawDecode> decode;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<ShapeStages> shape;
    std::shared_ptr<const DrawRecipe> recipe;
    Pm4::DrawParameters drawParameters;
    std::uint64_t drawKey = 0;
    std::uint64_t epoch = 0;
    std::uint32_t commitQueue = 0;
    std::uint32_t commitPacket = 0;
};

namespace {

struct DrawWorkPool {
    static constexpr std::size_t Kept = 1024;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<DrawWork*> works;
};

// Never destroyed: a commit may return its work while the process is going down.
DrawWorkPool& drawWorkPool() {
    static auto* pool = new DrawWorkPool();
    return *pool;
}

bool drawWorkPooled() {
    static const bool pooled = std::getenv("APS5_NO_DRAW_WORK_POOL") == nullptr;
    return pooled;
}

DrawWork* takeDrawWork() {
    DrawWork* work = nullptr;
    if (drawWorkPooled()) {
        auto& pool = drawWorkPool();
        while (pool.lock.test_and_set(std::memory_order_acquire)) {
        }
        if (!pool.works.empty()) {
            work = pool.works.back();
            pool.works.pop_back();
        }
        pool.lock.clear(std::memory_order_release);
    }
    return work != nullptr ? work : new DrawWork();
}

// Lets go of everything the draw shared with others (results, variants, the decode, the device)
// before the object waits for its next draw.
void returnDrawWork(DrawWork* work) {
    if (work == nullptr) return;
    if (!drawWorkPooled()) {
        delete work;
        return;
    }
    work->results.clear();
    work->matched.clear();
    work->fresh.clear();
    work->recipeStages.clear();
    work->stageKeyRefs.clear();
    for (auto& stage : work->stages) stage.contentKey = {};
    for (auto& capture : work->stageCaptures) capture.compiled.reset();
    work->localDevice.reset();
    work->decode.reset();
    work->entry.reset();
    work->shape.reset();
    work->recipe.reset();
    auto& pool = drawWorkPool();
    while (pool.lock.test_and_set(std::memory_order_acquire)) {
    }
    const bool kept = pool.works.size() < DrawWorkPool::Kept;
    if (kept) pool.works.push_back(work);
    pool.lock.clear(std::memory_order_release);
    if (!kept) delete work;
}

struct DrawWorkReturn {
    void operator()(DrawWork* work) const { returnDrawWork(work); }
};

}

// The rest of a draw after its decode, run on the queue worker or on a helper thread.
struct DrawTail {
    QueueState* queue = nullptr;
    std::uint32_t queueId = 0;
    std::uint32_t packetOpcode = 0;
    std::span<const std::uint32_t> packet;
    const Submission* submission = nullptr;
    std::string* rejected = nullptr;
    Pm4::DrawParameters drawParameters;
    bool pipelined = false;
    bool traceIndirect = false;
    std::unique_lock<GuestMemory::GpuMutexType>* gpuLock = nullptr;
    std::shared_ptr<VulkanDevice> localDevice;
    bool useDrawEntries = false;
    bool registerKey = false;
    std::uint64_t drawKey = 0;
    std::uint64_t shapeKey = 0;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    std::shared_ptr<ShapeStages> shape;
    // `decode` is the shape's own, shared by its draws: its programs hold another draw's user data words.
    bool sharedDecode = false;
    std::unique_ptr<DrawWork, DrawWorkReturn> work;
    std::array<double, DrawDriverPhaseCount>* phaseMs = nullptr;
    std::chrono::steady_clock::time_point* phaseLap = nullptr;
    std::uint64_t* captures = nullptr;
    bool helper = false;
    bool published = false;
    std::shared_ptr<DrawPipeline::PendingDraw> pending;
};

namespace {

// What a draw costs each of the two threads of the pipelined path, printed every 10 s as a
// [perf] line whatever the profiling switches say: a frame rate read off the screen moves by a
// tenth from run to run, and these averages over a hundred thousand draws do not. Two timestamp
// reads per draw and thread; each thread writes its own line of counters.
struct DrawPerf {
    struct alignas(64) Side {
        std::atomic<std::uint64_t> cycles{0};
        std::atomic<std::uint64_t> draws{0};
    };
    Side prepared;
    Side committed;
    alignas(64) std::uint64_t lastTsc = 0;
    long long lastQpc = 0;
    std::uint64_t lastPresents = 0;
    std::uint64_t lastWaited = 0;
    std::uint32_t untilCheck = 0;
};

DrawPerf& drawPerf() {
    static DrawPerf perf;
    return perf;
}

struct PreparedScope {
    std::uint64_t start = __rdtsc();
    std::uint64_t waitedBefore = DrawPipeline::FullWaitCycles().load(std::memory_order_relaxed);
    ~PreparedScope() {
        auto& perf = drawPerf();
        const auto now = __rdtsc();
        const auto waited = DrawPipeline::FullWaitCycles().load(std::memory_order_relaxed) - waitedBefore;
        perf.prepared.cycles.fetch_add(now - start - std::min(waited, now - start), std::memory_order_relaxed);
        perf.prepared.draws.fetch_add(1, std::memory_order_relaxed);
        if (++perf.untilCheck < 4096) return;
        perf.untilCheck = 0;
        LARGE_INTEGER qpc{}, frequency{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&frequency);
        if (perf.lastQpc == 0) {
            perf.lastQpc = qpc.QuadPart;
            perf.lastTsc = now;
            return;
        }
        const double seconds = static_cast<double>(qpc.QuadPart - perf.lastQpc) / static_cast<double>(frequency.QuadPart);
        if (seconds < 10.0) return;
        const double cyclesPerUs = static_cast<double>(now - perf.lastTsc) / (seconds * 1e6);
        const auto preparedDraws = perf.prepared.draws.exchange(0, std::memory_order_relaxed);
        const auto preparedCycles = perf.prepared.cycles.exchange(0, std::memory_order_relaxed);
        const auto committedDraws = perf.committed.draws.exchange(0, std::memory_order_relaxed);
        const auto committedCycles = perf.committed.cycles.exchange(0, std::memory_order_relaxed);
        const auto waitedNow = DrawPipeline::FullWaitCycles().load(std::memory_order_relaxed);
        const auto presents = VulkanDevice::PresentCounts().presents;
        const auto shown = presents - perf.lastPresents;
        std::fprintf(stderr, "[perf] %.1f s: %llu draws prepared in %.1f us each (and %.1f us each waiting for room in the queue), %llu committed in %.1f us each; %llu presents (%.1f a second, %.0f draws each); stages taken from their shape: %llu, %llu of them with the stage inputs checked again, %llu resolved; recorded with their shape's outputs %llu, layout %llu, pipeline %llu, resources %llu, targets %llu of %llu\n", seconds, static_cast<unsigned long long>(preparedDraws), preparedDraws != 0 ? static_cast<double>(preparedCycles) / cyclesPerUs / static_cast<double>(preparedDraws) : 0.0, preparedDraws != 0 ? static_cast<double>(waitedNow - perf.lastWaited) / cyclesPerUs / static_cast<double>(preparedDraws) : 0.0, static_cast<unsigned long long>(committedDraws), committedDraws != 0 ? static_cast<double>(committedCycles) / cyclesPerUs / static_cast<double>(committedDraws) : 0.0, static_cast<unsigned long long>(shown), static_cast<double>(shown) / seconds, shown != 0 ? static_cast<double>(preparedDraws) / static_cast<double>(shown) : 0.0, static_cast<unsigned long long>(StageMemoCounters().reused.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(StageMemoCounters().revalidated.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(StageMemoCounters().resolved.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().outputs.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().layouts.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().pipelines.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().resources.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().targets.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(Graphics::DrawShapeMemoCounters().draws.exchange(0, std::memory_order_relaxed)));
        std::fprintf(stderr, "[perf]   pixel stages replayed from their shape: %llu; captured instead: %llu with other user words, %llu with the same words over changed memory; stages of any kind captured from their shape without the walk: %llu\n", static_cast<unsigned long long>(StageMemoCounters().replayed.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(StageMemoCounters().replayOtherWords.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(StageMemoCounters().replayOtherMemory.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(StageMemoCounters().recaptured.exchange(0, std::memory_order_relaxed)));
        {
            auto& walked = StageMemoCounters().walked;
            const auto taken = [&](std::size_t kind, std::size_t reason) { return static_cast<unsigned long long>(walked[kind][reason].exchange(0, std::memory_order_relaxed)); };
            std::fprintf(stderr, "[perf]   stages walked instead (no capture kept / plan not taken over / other user words / changed memory): vertex %llu / %llu / %llu / %llu, pixel %llu / %llu / %llu / %llu\n", taken(0, 0), taken(0, 1), taken(0, 2), taken(0, 3), taken(1, 0), taken(1, 1), taken(1, 2), taken(1, 3));
        }
        perf.lastQpc = qpc.QuadPart;
        perf.lastTsc = now;
        perf.lastPresents = presents;
        perf.lastWaited = waitedNow;
    }
};

}

namespace {

// Measuring aid, APS5_NULL_DRAW_TEST=1: the driver alternates 20 s of normal work with 10 s in
// which every draw packet is dropped at once, and prints the presents of each such window. With
// the draws costing nothing, the frame rate of those windows is what the title's own threads and
// the rest of the driver (dispatches, flips, labels) allow: the ceiling any draw path work can
// reach. The picture is wrong during the windows.
bool nullDrawNow() {
    static const bool enabled = std::getenv("APS5_NULL_DRAW_TEST") != nullptr;
    if (!enabled) return false;
    constexpr double NormalSeconds = 20.0, NullSeconds = 10.0;
    static std::atomic<bool> dropping{false};
    static std::atomic<std::uint64_t> dropped{0};
    static std::atomic<std::uint32_t> untilCheck{0};
    static long long since = 0;
    static std::uint64_t presentsAtStart = 0;
    if (dropping.load(std::memory_order_relaxed)) dropped.fetch_add(1, std::memory_order_relaxed);
    if (untilCheck.fetch_add(1, std::memory_order_relaxed) % 64 == 0) {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (since == 0) since = now.QuadPart;
        const double seconds = static_cast<double>(now.QuadPart - since) / static_cast<double>(frequency.QuadPart);
        const bool isDropping = dropping.load(std::memory_order_relaxed);
        if (seconds >= (isDropping ? NullSeconds : NormalSeconds)) {
            const auto presents = VulkanDevice::PresentCounts().presents;
            if (isDropping) {
                const auto shown = presents - presentsAtStart;
                const auto count = dropped.exchange(0, std::memory_order_relaxed);
                std::fprintf(stderr, "[null-draws] %.1f s without draws: %llu draws dropped, %llu presents (%.1f a second, %.0f draws each)\n", seconds, static_cast<unsigned long long>(count), static_cast<unsigned long long>(shown), static_cast<double>(shown) / seconds, shown != 0 ? static_cast<double>(count) / static_cast<double>(shown) : 0.0);
            }
            presentsAtStart = presents;
            since = now.QuadPart;
            dropping.store(!isDropping, std::memory_order_relaxed);
        }
    }
    return dropping.load(std::memory_order_relaxed);
}

}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected) {
    if (nullDrawNow()) return DrawVerdict::Drawn;
    const PreparedScope prepared;
    PerformanceTimer timing("Driver.Draw");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    std::uint64_t captures = 0;
    auto phaseLap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    DrawPhaseTiming phaseTiming{profile, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[DrawRowPrologue] = std::chrono::duration<double, std::milli>(phaseLap - packetStartedAt()).count();

    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    auto drawParameters = Pm4::ResolveDraw(packet, queue);
    const bool pipelined = DrawPipeline::Active();
    if (pipelined && drawParameters.indirect) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Indirect);
    bool traceIndirect = false;
    if (const auto verdict = precheckDraw(queue, submission, packet, drawParameters, rejected, traceIndirect)) return *verdict;
    phaseTiming.Phase(DrawRowPrecheck);
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;

    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const bool lockedPrepare = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    std::shared_ptr<VulkanDevice> localDevice;
    if (lockedPrepare) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    } else if ((localDevice = device.Load()) == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard createLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_setup");
    phaseTiming.Phase(DrawRowVectors);

    const bool useDrawEntries = drawEntries() && !ShaderRecompiler::DebugProbeActive() && dumpTarget == 0 && dumpSlot1 == 0;
    const bool registerKey = useDrawEntries && registerKeyEnabled();
    std::uint64_t drawKey = 0, shapeKey = 0;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    if (registerKey) {
        const auto keyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        drawKey = drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), &shapeKey);
        std::lock_guard cacheLock(drawCacheMutex);
        ++drawEntryCounters.lookups;
        ++drawEntryCounters.registerKeyLookups;
        if (profile) drawEntryCounters.keyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count();
        const auto found = drawKeyFilter[drawKeySlot(drawKey)] != 0 ? drawCache.find(drawKey) : drawCache.end();
        if (found != drawCache.end()) {
            entry = found->second;
            decode = entry->decode;
        } else {
            ++drawEntryCounters.absent;
        }
    }
    static const bool profileKeys = std::getenv("APS5_PROFILE_DRAW_KEYS") != nullptr;
    if (profileKeys && registerKey) NoteDrawKeyRepeat(queue, drawKey);
    if (decode != nullptr && !DecodeProgramsCurrent(*decode, *submission.shaders)) {
        std::lock_guard cacheLock(drawCacheMutex);
        eraseDrawEntry(drawKey, entry);
        ++drawEntryCounters.absent;
        entry = nullptr;
        decode = nullptr;
    }
    phaseTiming.Phase(DrawRowKeyLookupValidate);

    // Without an entry for the whole key, a decode of the same registers apart from the user data
    // words (another frame's draw of the same object) saves decoding the state again.
    bool sharedDecode = false;
    std::shared_ptr<ShapeStages> shape;
    if (registerKey && decode == nullptr && !verifyDrawRecipe()) decode = shapeDecode(shapeKey, queue, submission, profile, &shape, &sharedDecode);
    const bool decodedAfresh = decode == nullptr;
    resolveDrawDecode(queue, submission, decode, registerKey, drawKey, profile);
    if (registerKey && decodedAfresh) keepShapeDecode(shapeKey, decode);
    const auto& graphics = decode->state;
    auto pixel = decode->pixel;
    std::unique_ptr<DrawWork, DrawWorkReturn> work(takeDrawWork());
    auto& programs = work->programs;
    programs = decode->programs;
    if (sharedDecode) readShapeUserWords(programs, queue);
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = programs.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    if (!drawParameters.indirect) setMeshIndexBuffer(drawParameters);
    else if (graphics.stages.mesh) setMeshIndexBuffer(Pm4::DrawParameters{drawParameters.indexAddress, std::max(drawParameters.indexCount, 1u), drawParameters.indexSize, 1, 0, drawParameters.indexed});
    const std::vector<Role>& roles = decode->roles;
    phaseTiming.Phase(DrawRowDecode);

    DrawTail tail;
    tail.queue = &queue;
    tail.queueId = submission.queue;
    tail.packetOpcode = (packet[0] >> 8u) & 0xffu;
    tail.packet = packet;
    tail.submission = &submission;
    tail.rejected = &rejected;
    tail.drawParameters = drawParameters;
    tail.pipelined = pipelined;
    tail.traceIndirect = traceIndirect;
    tail.gpuLock = &gpuLock;
    tail.localDevice = std::move(localDevice);
    tail.useDrawEntries = useDrawEntries;
    tail.registerKey = registerKey;
    tail.drawKey = drawKey;
    tail.shapeKey = shapeKey;
    tail.entry = std::move(entry);
    tail.decode = std::move(decode);
    tail.shape = std::move(shape);
    tail.sharedDecode = sharedDecode;
    tail.work = std::move(work);
    tail.phaseMs = &phaseMs;
    tail.phaseLap = &phaseLap;
    tail.captures = &captures;
    // APS5_PARALLEL_DRAWS=<n>: the rest of the draw (lookup, capture, recompile) runs on one of n
    // helper threads while this worker goes on with the next packets; the draw's place in the commit
    // order is taken here. The rest reads no registers: the helper is given an empty register set.
    if (DrawPipeline::Helpers() != 0 && pipelined && !tail.drawParameters.indirect && !lockedPrepare && dumpTarget == 0 && dumpSlot1 == 0 && deferredLabels().labels.empty()) {
        auto shared = std::make_shared<DrawTail>(std::move(tail));
        shared->helper = true;
        shared->pending = std::make_shared<DrawPipeline::PendingDraw>();
        auto& pipeline = DrawPipeline::Queue0();
        const auto seq = pipeline.EnqueuePending(shared->pending);
        const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        pipeline.RunOnHelper([this, shared, seq, epoch] { runDrawTailOnHelper(*shared, seq, epoch); });
        phaseTiming.Phase(DrawRowLockWait);
        return drawn();
    }
    return drawTail(tail);
}

DrawVerdict Driver::drawTail(DrawTail& t) {
    PerformanceTimer timing("Driver.DrawTail");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& phaseMs = *t.phaseMs;
    auto& phaseLap = *t.phaseLap;
    auto& captures = *t.captures;
    DrawPhaseTiming phaseTiming{profile, phaseMs, phaseLap};
    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile || t.helper) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();
    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();
    auto& queue = *t.queue;
    const auto& submission = *t.submission;
    auto& rejected = *t.rejected;
    auto& drawParameters = t.drawParameters;
    const bool pipelined = t.pipelined;
    const bool traceIndirect = t.traceIndirect;
    auto& gpuLock = *t.gpuLock;
    auto& localDevice = t.localDevice;
    const bool useDrawEntries = t.useDrawEntries;
    const bool registerKey = t.registerKey;
    auto& drawKey = t.drawKey;
    const auto shapeKey = t.shapeKey;
    auto& entry = t.entry;
    auto& decode = t.decode;
    auto& work = *t.work;
    auto& programs = work.programs;
    const auto& graphics = decode->state;
    auto pixel = decode->pixel;
    const std::vector<Role>& roles = decode->roles;
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = programs.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };

    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        if (location == 0x280u) return std::nullopt;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::Fragment || roles[i] == Role::GeometryBack || location < programs[i].userDataBase) continue;
            const auto word = location - programs[i].userDataBase + (8u - programs[i].firstUserSgpr);
            if (word < programs[i].userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
        }
        return std::nullopt;
    };
    if (drawParameters.indirect) {
        auto& indirect = *drawParameters.indirect;
        const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
            const auto word = locate(location);
            if (!word || word->first != 0) return -1;
            return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
        };
        indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
        indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
        indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
    }
    auto& memory = work.memory;
    auto& linked = work.linked;
    memory.clear();
    linked.clear();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (const auto& region : program.memory) {
            if (!region.bytes.empty()) memory.push_back(region);
        }
        linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    timing.Mark("prepare");
    phaseTiming.Phase(DrawRowProgramPrepare);

    auto& vertexInfos = work.vertexInfos;
    vertexInfos.assign(programs.size(), std::nullopt);
    auto& decodeReads = work.decodeReads;
    decodeReads.resize(programs.size());
    for (auto& reads : decodeReads) reads.clear();
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        decodeReads[i].clear();
        vertexInfos[i] = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decodeReads[i], false, 8u - program.firstUserSgpr);
        if (vertexInfos[i]) vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, readRegister(queue.context, 0x207));
    };
    if (!registerKey) {
        for (std::size_t i = 0; i < programs.size(); ++i) decodeVertexInfo(i);
        phaseTiming.Phase(DrawRowDecode);
    }
    auto& shaderMemory = work.shaderMemory;
    shaderMemory.Reset(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    auto& results = work.results;
    auto& stages = work.stages;
    results.clear();
    stages.clear();
    results.reserve(programs.size() + 2u);
    stages.reserve(programs.size());
    std::uint32_t pushCursorBytes = 0;

    auto& programResults = work.programResults;
    programResults.assign(programs.size(), nullptr);

    auto& stageCaptures = work.stageCaptures;
    stageCaptures.resize(programs.size());
    for (auto& capture : stageCaptures) {
        capture.compiled.reset();
        capture.regions.clear();
        capture.forgetSerial = 0;
        capture.pushOffset = 0;
    }
    auto& matched = work.matched;
    matched.assign(programs.size(), nullptr);
    auto& matchedRegions = work.matchedRegions;
    matchedRegions.resize(programs.size());
    for (auto& regions : matchedRegions) regions.clear();

    auto& fresh = work.fresh;
    fresh.assign(programs.size(), nullptr);

    auto& recompiled = work.recompiled;
    recompiled.assign(programs.size(), false);
    bool drawHit = false;
    bool verifyHit = false;
    // The shape's stage records are this draw's until it returns.
    struct ShapeHold {
        ShapeStages* shape;
        ~ShapeHold() {
            if (shape != nullptr) shape->busy.clear(std::memory_order_release);
        }
    } shapeHold{t.shape != nullptr && !t.shape->busy.test_and_set(std::memory_order_acquire) ? t.shape.get() : nullptr};
    if (shapeHold.shape != nullptr && shapeHold.shape->stages.size() != programs.size()) {
        shapeHold.shape->stages.clear();
        shapeHold.shape->stages.resize(programs.size());
    }
    std::uint64_t pixelFrom = 0;
    lookupDraw(submission, localDevice, graphics, pixel, programs, roles, vertexInfos, useDrawEntries, registerKey, profile, drawKey, entry, matched, matchedRegions, drawHit, verifyHit, phaseTiming, phaseMs);

    if (registerKey) {

        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (matched[i] != nullptr && drawHit && !verifyDrawRecipe()) {
                if (matched[i]->vertexInfo != nullptr) vertexInfos[i] = *matched[i]->vertexInfo;
                continue;
            }
            decodeVertexInfo(i);
            if (matched[i] != nullptr && verifyDrawRecipe() && (vertexInfos[i].has_value() != (matched[i]->vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *matched[i]->vertexInfo)))) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit has a vertex stage info unlike its variant's\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress));
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.verifyDecodeMismatches;
            }
        }
        phaseTiming.Phase(DrawRowDecode);
    }

    const auto fold = [&](const ShaderRecompiler::RecompileResult& main, Pm4::DrawParameters& parameters) { FoldDrawOffsets(main, programs.front(), parameters); };

    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& pushOffsets = work.pushOffsets;
    pushOffsets.assign(programs.size(), 0);
    auto& resultIndex = work.resultIndex;
    resultIndex.assign(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (roles[i] == Role::GeometryBack) continue;
        const auto& program = programs[i];
        pushCursorBytes = Graphics::StagePushOffset(pushCursorBytes, program.binary.stage, localDevice->GraphicsPipelineLibraries());
        pushOffsets[i] = pushCursorBytes;
        if (drawHit) {

            programResults[i] = matched[i]->compiled.get();
            memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
        } else {
            resultIndex[i] = results.size();
            if (program.binary.stage == ShaderRecompiler::ShaderStage::Fragment) {
                for (auto previous = i; previous-- > 0;) {
                    if (programResults[previous] == nullptr) continue;
                    pixel = Graphics::DefaultUnexportedInputs(decode->pixel, programResults[previous]->parameterExports);
                    pixelFrom = programResults[previous]->variantId;
                    break;
                }
            }
            results.push_back(materializeDrawStage(i, pushCursorBytes, queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, shapeHold.shape, pixelFrom));
            if (!rejected.empty()) return DrawVerdict::Rejected;
            programResults[i] = results.back().get();
        }
        const auto& result = *programResults[i];
        if (i == 0 && drawParameters.indirect) {
            indirectCpu = ClassifyIndirectDraw(result, graphics, programs.front(), localDevice, drawParameters, traceIndirect);
        } else if (i == 0) {
            fold(result, drawParameters);
        }
        const auto slotEnd = (pushCursorBytes / Graphics::PipelinePushSlotBytes + 1u) * Graphics::PipelinePushSlotBytes;
        require(result.pushConstants.size() <= slotEnd - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
        stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes});
        pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
    }
    if (shapeHold.shape != nullptr && !drawHit && ReplayProbeEnabled()) ProbeReplay(*shapeHold.shape, programs, roles, stageCaptures, programResults, drawParameters, graphics);

    // Titles that move a draw's constant buffers every frame (Unity) never find most entries again;
    // building them, and their recipes, is skipped for such shapes.
    const bool insertEntry = useDrawEntries && (!registerKey || keepDrawEntry(shapeKey, drawHit));
    // An entry keeps a decode with its own draw's user data words.
    std::shared_ptr<const DrawDecode> entryDecode = decode;
    if (insertEntry && t.sharedDecode && !drawHit) {
        auto own = std::make_shared<DrawDecode>(*decode);
        for (std::size_t i = 0; i < programs.size(); ++i) own->programs[i].userData = programs[i].userData;
        entryDecode = std::move(own);
    }
    cacheDrawStages(insertEntry, drawHit, drawParameters, indirectCpu, programs, stageCaptures, vertexInfos, decodeReads, verifyHit, matched, fresh, drawKey, registerKey, entryDecode, phaseTiming);
    timing.Mark("shader_compile_and_link");

    for (const auto& reads : decodeReads) {
        for (const auto& read : reads) memory.push_back({read.address, std::as_bytes(std::span(read.bytes))});
    }

    if (pipelined && !deferredLabels().labels.empty()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Labels);
    if (!t.helper && recordQueuedLabelsAfterCapture(submission.queue, memory)) return draw(queue, t.packet, submission, rejected);

    bool rectListBuilt = false;

    // A result replaced in `results` (shared, so the old object goes away with it) is replaced in the
    // stages and program results that point at it.
    const auto replaceResult = [&](std::size_t index, std::shared_ptr<const ShaderRecompiler::RecompileResult> now) {
        const auto* old = results[index].get();
        for (auto& stage : stages) {
            if (stage.program == old) stage.program = now.get();
        }
        for (auto& program : programResults) {
            if (program == old) program = now.get();
        }
        results[index] = std::move(now);
    };
    std::size_t rectIndex = 0;
    const auto buildRectList = [&] {
        phaseTiming.Phase(DrawRowVectors);
        require(programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr, "rect-list requires vertex and fragment programs");
        auto rectangle = DrawRectangle(*programs[0].snapshot, programs[1].snapshot, programResults[0]->variantId, programResults[1]->variantId, localDevice->Target());
        auto control = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.control));
        auto evaluation = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.evaluation));
        if (rectListBuilt) {
            replaceResult(rectIndex, std::move(control));
            replaceResult(rectIndex + 1, std::move(evaluation));
            phaseTiming.Phase(DrawRowRectList);
            return;
        }
        require(stages.size() == 2, "rect-list requires vertex and fragment programs");
        rectIndex = results.size();
        stages.insert(stages.begin() + 1, {{Stage::TessellationControl, control.get(), 0}, {Stage::TessellationEvaluation, evaluation.get(), 0}});
        results.push_back(std::move(control));
        results.push_back(std::move(evaluation));
        rectListBuilt = true;
        phaseTiming.Phase(DrawRowRectList);
    };
    if (graphics.rectList) buildRectList();
    if (!programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active) {
        require(!graphics.rectList && graphics.stages.path == Graphics::ShaderPath::Vertex && programs.size() == 2 && programResults[0] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric needs a vertex shader before it");
        results.push_back(std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::BuildBarycentricGeometryShader(*programResults[0], *programResults[1], localDevice->Target(), localDevice->GeometryLimits())));
        stages.insert(stages.begin() + 1, Graphics::CompiledShader{Stage::Geometry, results.back().get(), 0});
    }
    auto& snapshots = work.snapshots;
    const auto snapshot = [&] {
        snapshots.clear();
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
    };
    snapshot();
    timing.Mark("post_compile_prepare");
    const auto lockForDraw = [&] {
        if (gpuLock.owns_lock()) return;
        phaseTiming.Phase(DrawRowVectors);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);

        if (auto current = device.Load(); current != nullptr && current != localDevice) {
            static std::atomic<std::uint64_t> replaced{0};
            std::fprintf(stderr, "[draw] device replaced during unlocked preparation (%llu)\n", static_cast<unsigned long long>(++replaced));
            localDevice = std::move(current);
        }

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    };
    if (drawParameters.indirect && indirectCpu) {

        const auto indirect = *drawParameters.indirect;
        if (drawHit) {

            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (programResults[i] == nullptr) continue;
                resultIndex[i] = results.size();
                results.push_back(matched[i]->compiled);
            }
        }
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
        std::vector<Pm4::DrawArguments> records;
        for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
        Graphics::CountIndirectDraw(*indirectCpu, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        const auto baseVertexWord = locate(indirect.baseVertexLocation);
        const auto startInstanceWord = locate(indirect.startInstanceLocation);
        const auto drawIndexWord = indirect.drawIndexEnabled ? locate(indirect.drawIndexLocation) : std::nullopt;
        for (std::uint32_t record = 0; record < records.size(); ++record) {
            const auto& arguments = records[record];
            if (traceIndirect) std::fprintf(stderr, "[draw]   record %u: count %u instances %u first %u vertexOffset %u startInstance %u\n", record, arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance);
            if (arguments.count == 0 || arguments.instances == 0) continue;
            std::set<std::size_t> patched;
            const auto patch = [&](const std::optional<std::pair<std::size_t, std::size_t>>& word, std::uint32_t value) {
                if (!word) return;
                programs[word->first].userData[word->second] = value;
                patched.insert(word->first);
            };
            patch(baseVertexWord, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
            patch(startInstanceWord, arguments.firstInstance);
            patch(drawIndexWord, record);
            Pm4::DrawParameters direct{0, arguments.count, 0, arguments.instances, drawParameters.flags, drawParameters.indexed, 0, 0};
            if (drawParameters.indexed) {

                if (arguments.firstVertexOrIndex >= drawParameters.indexCount) continue;
                direct.indexAddress = drawParameters.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * drawParameters.indexSize;
                direct.indexCount = std::min(arguments.count, drawParameters.indexCount - arguments.firstVertexOrIndex);
                direct.indexSize = drawParameters.indexSize;
            } else {
                direct.firstVertex = indirect.indxOffset;
            }
            if (graphics.stages.mesh) {
                setMeshIndexBuffer(direct);
                patched.insert(0);
            }
            for (const auto programIndex : patched) {
                const auto pushBytes = results[resultIndex[programIndex]]->pushConstants.size();
                decodeVertexInfo(programIndex);
                auto result = materializeDrawStage(programIndex, pushOffsets[programIndex], queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, shapeHold.shape, pixelFrom);
                if (!rejected.empty()) return DrawVerdict::Rejected;
                require(result->pushConstants.size() == pushBytes, "patched program changed its push constant layout");
                replaceResult(resultIndex[programIndex], std::move(result));
            }
            fold(*programResults[0], direct);
            if (graphics.rectList && patched.contains(0)) buildRectList();
            snapshot();
            if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
                rejected = std::move(*known);
                return DrawVerdict::Rejected;
            }
            lockForDraw();
            noteDrawWriters(stages, submission.queue);
            phaseTiming.Phase(DrawRowVectors);
            localDevice->Draw(graphics, direct, stages, snapshots);
            phaseTiming.Phase(DrawRowGraphics);
        }
        timing.Mark("draw_and_resource_release");
        return drawn();
    }

    auto& recipeStages = work.recipeStages;
    recipeStages.clear();
    if (registerKey && !drawParameters.indirect && Graphics::DrawRecipes()) {
        recipeStages.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) recipeStages.push_back(drawHit ? matched[i] : fresh[i]);
        if (std::all_of(recipeStages.begin(), recipeStages.end(), [](const std::shared_ptr<DispatchVariant>& variant) { return variant == nullptr; })) recipeStages.clear();
    }
    std::shared_ptr<const DrawRecipe> recipe;
    if (drawHit && !recipeStages.empty()) {
        recipe = findDrawRecipe(drawKey, recipeStages);
        if (recipe == nullptr) VulkanDevice::NoteDrawRecipeMiss(VulkanDevice::DrawRecipePrecheck::NoRecipe);
    }
    if (recipe == nullptr) {
        if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
            rejected = std::move(*known);
            return DrawVerdict::Rejected;
        }
    }
    if (t.helper || (pipelined && !drawParameters.indirect && deferredLabels().labels.empty())) {
        phaseTiming.Phase(DrawRowVectors);
        // The stages' parts of the resource cache key are built here instead of on the recording
        // thread, which then compares bytes (Graphics::CompiledShader::contentKey). A replayed
        // fragment result's part is built once and shared. APS5_NO_PREPARED_RESOURCE_KEYS=1 leaves
        // the key to the recording thread, as before.
        static const bool preparedKeys = std::getenv("APS5_NO_PREPARED_RESOURCE_KEYS") == nullptr;
        if (preparedKeys && std::all_of(stages.begin(), stages.end(), [](const Graphics::CompiledShader& stage) { return stage.program != nullptr; })) {
            if (work.stageKeys.size() < stages.size()) work.stageKeys.resize(stages.size());
            work.stageKeyRefs.assign(stages.size(), nullptr);
            for (std::size_t k = 0; k < stages.size(); ++k) {
                auto& stage = stages[k];
                // The shape's record of the stage, when the stage is one of the draw's programs.
                ShapeStage* recall = nullptr;
                if (shapeHold.shape != nullptr) {
                    for (std::size_t i = 0; i < programs.size() && i < shapeHold.shape->stages.size(); ++i) {
                        if (programResults[i] == stage.program) recall = &shapeHold.shape->stages[i];
                    }
                }
                if (recall == nullptr) {
                    Graphics::DrawStageContentKey(stage, work.stageKeys[k]);
                    stage.contentKey = work.stageKeys[k];
                    continue;
                }
                if (recall->replayResult.get() == stage.program) {
                    if (recall->replayKey == nullptr || recall->replayKeyOf != recall->replayResult) {
                        auto built = std::make_shared<std::vector<std::uint32_t>>();
                        Graphics::DrawStageContentKey(stage, *built);
                        recall->replayKey = std::move(built);
                        recall->replayKeyOf = recall->replayResult;
                    }
                    work.stageKeyRefs[k] = recall->replayKey;
                    stage.contentKey = *recall->replayKey;
                    continue;
                }
                const auto* plan = stage.program->bindingPlan.Plan();
                const auto variant = stage.program->PipelineVariantId();
                const bool fixedKnown = plan != nullptr && plan == recall->lastKeyPlan && variant == recall->lastKeyVariant && stage.stage == recall->lastKeyStage;
                if (recall->lastKey == nullptr || !Graphics::DrawStageContentKeyIs(stage, *recall->lastKey, fixedKnown)) {
                    auto built = std::make_shared<std::vector<std::uint32_t>>();
                    Graphics::DrawStageContentKey(stage, *built);
                    recall->lastKey = std::move(built);
                    recall->lastKeyPlan = plan;
                    recall->lastKeyVariant = variant;
                    recall->lastKeyStage = stage.stage;
                }
                work.stageKeyRefs[k] = recall->lastKey;
                stage.contentKey = *recall->lastKey;
            }
        }
        auto writes = drawWriteRanges(graphics, stages);
        const auto commitQueue = submission.queue;
        const auto commitPacket = t.packetOpcode;
        const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        // The commit reads everything from the draw's work and returns it when it is done.
        work.localDevice = std::move(localDevice);
        work.decode = std::move(decode);
        work.entry = std::move(entry);
        work.shape = t.shape;
        work.recipe = std::move(recipe);
        work.drawParameters = drawParameters;
        work.drawKey = drawKey;
        work.epoch = epoch;
        work.commitQueue = commitQueue;
        work.commitPacket = commitPacket;
        const auto commit = [this, work = &work] {
            const std::unique_ptr<DrawWork, DrawWorkReturn> returned(work);
            DrawPipeline::FollowEpoch(work->epoch);
            GuestMemory::SetCurrentPacket(work->commitPacket, work->commitQueue);
            const auto commitStart = __rdtsc();
            commitDraw(work->localDevice, work->commitQueue, work->decode->state, work->drawParameters, work->stages, work->snapshots, work->recipe, work->recipeStages, work->drawKey, work->shape != nullptr ? &work->shape->drawn : nullptr);
            drawPerf().committed.cycles.fetch_add(__rdtsc() - commitStart, std::memory_order_relaxed);
            drawPerf().committed.draws.fetch_add(1, std::memory_order_relaxed);
        };
        // The queue owns the work once it holds the commit (a failure it reports first leaves it here).
        if (t.helper) {
            DrawPipeline::Queue0().Publish(t.pending, commit, std::move(writes));
            t.published = true;
        } else {
            DrawPipeline::Queue0().Enqueue(commit, std::move(writes));
        }
        static_cast<void>(t.work.release());
        phaseTiming.Phase(DrawRowLockWait);
        timing.Mark("draw_enqueued");
        return drawn();
    }
    lockForDraw();
    noteDrawWriters(stages, submission.queue);
    phaseTiming.Phase(DrawRowVectors);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe) == RecipeOutcome::Recorded) {
            phaseTiming.Phase(DrawRowGraphics);
            timing.Mark("draw_and_resource_release");
            return drawn();
        }

        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, t.shape != nullptr ? &t.shape->drawn : nullptr);
    phaseTiming.Phase(DrawRowGraphics);
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built));
    timing.Mark("draw_and_resource_release");
    return drawn();
}

void Driver::runDrawTailOnHelper(DrawTail& tail, std::uint64_t seq, std::uint64_t epoch) {
    DrawPipeline::Active() = true;
    DrawPipeline::CurrentSeq() = seq;
    DrawPipeline::FollowEpoch(epoch);
    GuestMemory::SetCurrentPacket(tail.packetOpcode, tail.queueId);
    thread_local Submission* scratchSlot = nullptr;
    auto& scratch = ShaderRecompiler::ThreadOwned(scratchSlot);
    scratch.queue = tail.queueId;
    thread_local QueueState* queueSlot = nullptr;
    tail.queue = &ShaderRecompiler::ThreadOwned(queueSlot);
    tail.submission = &scratch;
    std::string rejected;
    tail.rejected = &rejected;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    auto phaseLap = std::chrono::steady_clock::now();
    std::uint64_t captures = 0;
    tail.phaseMs = &phaseMs;
    tail.phaseLap = &phaseLap;
    tail.captures = &captures;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    tail.gpuLock = &gpuLock;
    // The draw is published whatever happens, so the commits after it are not held up; a draw that
    // cannot run is skipped and reported once per reason, as on the worker.
    try {
        drawTail(tail);
        if (!rejected.empty()) reportSkip("draw", rejected + " [prepared on a helper]");
    } catch (const ProcessShutdown&) {
    } catch (const std::exception& error) {
        reportSkip("draw", std::string(error.what()) + " [prepared on a helper]");
    } catch (...) {
    }
    if (gpuLock.owns_lock()) gpuLock.unlock();
    if (!tail.published) DrawPipeline::Queue0().Publish(tail.pending, {}, {});
    DrawPipeline::CurrentSeq() = 0;
}


void Driver::commitDraw(std::shared_ptr<VulkanDevice> localDevice, std::uint32_t queue, const Graphics::State& graphics, const Pm4::DrawParameters& drawParameters, std::span<const Graphics::CompiledShader> stages, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe, const std::vector<std::shared_ptr<DispatchVariant>>& recipeStages, std::uint64_t drawKey, Graphics::DrawShapeMemo* memo) {
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (auto current = device.Load(); current != nullptr && current != localDevice) localDevice = std::move(current);
    struct SubmitDue {
        Driver& driver;
        const std::shared_ptr<VulkanDevice>& device;
        ~SubmitDue() noexcept(false) {
            if (std::uncaught_exceptions() == 0) driver.submitDueAfterCommit(device.get());
        }
    } submitDue{*this, localDevice};
    recordLabelsForPacket(localDevice.get(), queue);
    noteDrawWriters(stages, queue);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe) == RecipeOutcome::Recorded) return;
        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, memo);
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built));
}

}
