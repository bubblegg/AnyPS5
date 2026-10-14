#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

bool runtimeRegister(std::uint32_t offset) {
    return offset - 0x00cu < 32u || offset - 0x08cu < 32u || offset - 0x10cu < 32u || offset == 0x082u || offset == 0x083u || offset == 0x102u || offset == 0x103u;
}

const auto& registerMasks() {
    static const auto masks = [] {
        std::array<std::vector<std::uint64_t>, 3> bits;
        for (const auto& range : Graphics::DrawKeyRegisters) {
            auto& bank = bits[static_cast<std::size_t>(range.bank)];
            bank.resize(std::max(bank.size(), (static_cast<std::size_t>(range.first) + range.count + 63) / 64));
            for (auto offset = range.first; offset < range.first + range.count; ++offset) {
                if (range.bank == Graphics::RegisterBank::Shader && runtimeRegister(offset)) continue;
                bank[offset / 64] |= std::uint64_t{1} << (offset % 64);
            }
        }
        std::array<std::shared_ptr<const Registers::Mask>, 3> result;
        for (std::size_t i = 0; i < bits.size(); ++i) result[i] = std::make_shared<const Registers::Mask>(std::move(bits[i]));
        return result;
    }();
    return masks;
}

}

// `shapeKey`, when given, receives the key over everything but the shader user data words: the
// fingerprints and the other runtime registers (0x082, 0x083, 0x102, 0x103). The draws of a shape
// differ in their user data words alone (Driver::shapeDecode and the records kept by shape).
std::uint64_t RegisterStateKey(const QueueState& queue, bool allUserWords, bool incremental, std::uint64_t* shapeKey) {
    const auto& masks = registerMasks();
    const auto fingerprint = [&](const Registers& registers, Graphics::RegisterBank bank) {
        const auto& mask = masks[static_cast<std::size_t>(bank)];
        return incremental ? registers.Fingerprint(mask) : registers.RecomputeFingerprint(*mask);
    };
    auto shape = (0xcbf29ce484222325ull ^ fingerprint(queue.context, Graphics::RegisterBank::Context)) * 0x100000001b3ull;
    shape = (shape ^ fingerprint(queue.shader, Graphics::RegisterBank::Shader)) * 0x100000001b3ull;
    shape = (shape ^ fingerprint(queue.userConfig, Graphics::RegisterBank::UserConfig)) * 0x100000001b3ull;
    const auto words = [&](std::uint64_t& into, std::uint32_t base, std::uint32_t count) {
        for (auto it = queue.shader.lower_bound(base); it != queue.shader.end() && it->first < base + count; ++it) {
            into = (into ^ it->first) * 0x100000001b3ull;
            into = (into ^ it->second) * 0x100000001b3ull;
        }
    };
    words(shape, 0x082u, 2);
    words(shape, 0x102u, 2);
    if (shapeKey != nullptr) *shapeKey = shape;
    auto exact = shape;
    for (const auto base : {0x00cu, 0x08cu, 0x10cu}) {
        const auto resources = queue.shader.find(base - 1);
        const auto count = resources == queue.shader.end() ? 0u : ((resources->second >> 1u) & 0x1fu) | (((resources->second >> 27u) & 1u) << 5u);
        words(exact, base, allUserWords ? 32u : std::min(count, 32u));
    }
    return exact;
}

// The context ranges used to be walked (some 400 registers) and their hash kept with a copy of the
// bank; upstream's incremental fingerprints (Registers::Fingerprint) replace both.
std::uint64_t Driver::drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial, std::uint64_t* shapeKey) {
    static const bool allUserWords = std::getenv("APS5_DRAW_KEY_ALL_USER_WORDS") != nullptr;
    static const bool verify = std::getenv("APS5_VERIFY_REGISTER_KEYS") != nullptr;
    std::uint64_t shape = 0;
    auto key = RegisterStateKey(queue, allUserWords, true, &shape);
    if (verify && key != RegisterStateKey(queue, allUserWords, false)) throw std::runtime_error("incremental register fingerprint disagrees with full state");
    // The device and the programs belong to the shape as well as to the key.
    const auto mix = [&](std::uint64_t value) {
        key = (key ^ value) * 0x100000001b3ull;
        shape = (shape ^ value) * 0x100000001b3ull;
    };
    mix(deviceSerial);
    for (const auto base : {0x008u, 0x088u, 0x0c8u, 0x108u, 0x148u}) {
        const auto low = queue.shader.find(base);
        const auto high = queue.shader.find(base + 1);
        if (low == queue.shader.end() || high == queue.shader.end()) {
            mix(0);
            continue;
        }
        const auto address = (static_cast<std::uint64_t>(low->second) << 8u) | (static_cast<std::uint64_t>(high->second & 0xffu) << 40u);
        auto it = registry.upper_bound(address);
        if (it == registry.begin()) {
            mix(1);
            continue;
        }
        --it;
        for (const auto& snapshot : it->second) mix(reinterpret_cast<std::uintptr_t>(snapshot.get()));
        mix(address - it->first);
    }
    if (shapeKey != nullptr) *shapeKey = shape;
    return key;
}

std::shared_ptr<const DrawDecode> Driver::shapeDecode(std::uint64_t shapeKey, const QueueState& queue, const Submission& submission, bool profile, std::shared_ptr<ShapeStages>* stages, bool* shared) {
    static const bool disabled = std::getenv("APS5_NO_SHAPE_DECODE") != nullptr;
    static const bool sharing = std::getenv("APS5_NO_SHARED_SHAPE_DECODE") == nullptr;
    static const bool stageMemo = std::getenv("APS5_NO_STAGE_MEMO") == nullptr;
    static const bool verify = std::getenv("APS5_VERIFY_SHAPE_DECODE") != nullptr;
    if (disabled) return nullptr;
    std::shared_ptr<const DrawDecode> cached;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        if (const auto found = drawShapes.find(shapeKey); found != drawShapes.end()) {
            cached = found->second.decode;
            if (stages != nullptr && stageMemo && cached != nullptr) *stages = found->second.stages;
        }
        if (profile) ++(cached != nullptr ? drawEntryCounters.shapeDecodeHits : drawEntryCounters.shapeDecodeMisses);
    }
    if (cached == nullptr) return nullptr;
    // The draws of a shape share its decode: a draw copied it (the state, the programs and their
    // vectors) to write its own user data words into the copy, and the thread that records the
    // draw destroyed the copy. The caller reads the words into its own programs instead
    // (readShapeUserWords). A mesh shape keeps its copy (its hidden user words are the draw's).
    // APS5_NO_SHARED_SHAPE_DECODE=1 copies as before.
    if (sharing && shared != nullptr && !verify && !cached->state.stages.mesh && DecodeProgramsCurrent(*cached, *submission.shaders)) {
        *shared = true;
        return cached;
    }
    // The register state and the pixel stage read no user data words. The programs are those of the
    // shape too (the key holds their snapshots, offsets and merged-stage pointers), so only their user
    // data words are read again; a raw program rewritten since is decoded afresh.
    std::shared_ptr<DrawDecode> decode;
    if (DecodeProgramsCurrent(*cached, *submission.shaders)) {
        decode = std::make_shared<DrawDecode>(*cached);
        readShapeUserWords(decode->programs, queue);
    } else {
        decode = std::make_shared<DrawDecode>();
        decode->state = cached->state;
        decode->pixel = cached->pixel;
        DecodeGraphicsPrograms(*decode, queue, *submission.shaders, false, true);
    }
    if (verify) {
        const auto fresh = decodeDraw(queue, submission);
        if (!sameDecode(*decode, *fresh)) {
            static std::atomic<std::uint64_t> reports{0};
            if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: a decode reused by shape 0x%llx differs from a fresh decode (target 0x%llx); the fresh one is used\n", static_cast<unsigned long long>(shapeKey), static_cast<unsigned long long>(fresh->state.color.address));
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.shapeDecodeMismatches;
            return fresh;
        }
    }
    return decode;
}

void Driver::readShapeUserWords(std::vector<DrawProgram>& programs, const QueueState& queue) {
    for (auto& program : programs) {
        if (program.userData.empty()) continue;
        const auto found = queue.shader.find(program.userDataBase - 1);
        const auto resources = found == queue.shader.end() ? 0u : found->second;
        const auto count = std::min<std::size_t>(((resources >> 1u) & 0x1fu) | (((resources >> 27u) & 1u) << 5u), program.userData.size());
        const auto first = program.userData.size() - count;
        for (std::size_t i = 0; i < count; ++i) program.userData[first + i] = readUserData(queue.shader, program.userDataBase + static_cast<std::uint32_t>(i));
    }
}

void Driver::keepShapeDecode(std::uint64_t shapeKey, const std::shared_ptr<const DrawDecode>& decode) {
    constexpr std::size_t MaxShapes = 1u << 15u;
    std::lock_guard cacheLock(drawCacheMutex);
    if (drawShapes.size() >= MaxShapes && !drawShapes.contains(shapeKey)) drawShapes.clear();
    auto& record = drawShapes[shapeKey];
    record.decode = decode;
    record.stages = std::make_shared<ShapeStages>();
}

bool Driver::keepDrawEntry(std::uint64_t shapeKey, bool drawHit) {
    static const bool always = std::getenv("APS5_ALWAYS_INSERT_DRAWS") != nullptr;
    constexpr std::uint32_t Learning = 16, ProbeEvery = 64, HitShare = 8;
    std::lock_guard cacheLock(drawCacheMutex);
    const auto found = drawShapes.find(shapeKey);
    if (found == drawShapes.end()) return true;
    auto& record = found->second;
    if (record.draws >= (1u << 30u)) record.draws = record.entryHits = 0;
    ++record.draws;
    if (drawHit) ++record.entryHits;
    if (always || drawHit || record.draws <= Learning || record.entryHits * HitShare >= record.draws || record.draws % ProbeEvery == 0) return true;
    ++drawEntryCounters.insertsSkipped;
    return false;
}

namespace {

// Profiling aid (APS5_PROFILE_DRAW_KEYS): whether the draws of a frame repeat those of the frame before,
// by the whole register key and by the key without the user data words (the "shape"), and which
// user words differ between the n-th draws of the same shape in two frames. Read at each flip.
struct KeyRepeats {
    using UserWords = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    std::mutex mutex;
    std::unordered_set<std::uint64_t> previousKeys, keys;
    std::unordered_map<std::uint64_t, std::vector<UserWords>> previousShapes, shapes;
    std::map<std::uint32_t, std::uint64_t> differingWords;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> lastChange;
    std::uint64_t draws = 0, keyRepeats = 0, shapeRepeats = 0, frames = 0, distinct = 0, sameShapeOtherKey = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

KeyRepeats& keyRepeats() {
    static auto* repeats = new KeyRepeats();
    return *repeats;
}

}

void NoteDrawKeyRepeat(const QueueState& queue, std::uint64_t drawKey) {
    std::uint64_t shape = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        shape ^= value;
        shape *= 0x100000001b3ull;
    };
    KeyRepeats::UserWords users;
    for (const auto& range : Graphics::DrawKeyRegisters) {
        const auto& bank = range.bank == Graphics::RegisterBank::Context ? queue.context : range.bank == Graphics::RegisterBank::Shader ? queue.shader : queue.userConfig;
        const bool shader = range.bank == Graphics::RegisterBank::Shader;
        const auto end = range.first + range.count;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            const auto offset = it->first;
            const bool user = shader && ((offset >= 0x00cu && offset < 0x02cu) || (offset >= 0x08cu && offset < 0x0acu) || (offset >= 0x10cu && offset < 0x12cu));
            if (user) {
                users.emplace_back(offset, it->second);
                continue;
            }
            mix((static_cast<std::uint64_t>(range.bank) << 32u) | offset);
            mix(it->second);
        }
    }
    auto& r = keyRepeats();
    std::lock_guard lock(r.mutex);
    ++r.draws;
    if (r.previousKeys.contains(drawKey)) ++r.keyRepeats;
    if (r.keys.insert(drawKey).second) ++r.distinct;
    auto& occurrences = r.shapes[shape];
    const auto index = occurrences.size();
    if (const auto previous = r.previousShapes.find(shape); previous != r.previousShapes.end()) {
        ++r.shapeRepeats;
        if (!r.previousKeys.contains(drawKey) && index < previous->second.size()) {
            ++r.sameShapeOtherKey;
            const auto& before = previous->second[index];
            for (const auto& [offset, value] : users) {
                const auto old = std::find_if(before.begin(), before.end(), [&](const auto& word) { return word.first == offset; });
                if (old != before.end() && old->second == value) continue;
                ++r.differingWords[offset];
                r.lastChange[offset] = {old != before.end() ? old->second : 0u, value};
            }
        }
    }
    occurrences.push_back(std::move(users));
}

void DrawKeysAtFlip() {
    auto& r = keyRepeats();
    std::lock_guard lock(r.mutex);
    if (r.draws == 0) return;
    ++r.frames;
    r.previousKeys.swap(r.keys);
    r.keys.clear();
    r.previousShapes.swap(r.shapes);
    r.shapes.clear();
    const auto now = std::chrono::steady_clock::now();
    if (now - r.reported < std::chrono::seconds(10)) return;
    r.reported = now;
    const auto percent = [&](std::uint64_t part) { return 100.0 * static_cast<double>(part) / static_cast<double>(r.draws); };
    std::fprintf(stderr, "[draw-key] %llu frames, %llu draws (%llu per frame, %llu distinct keys per frame): whole key seen the frame before %.1f%%, key without user words %.1f%%; same shape with another key %llu; user words that differed (shader register: draws, last old->new):",
                 static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.draws), static_cast<unsigned long long>(r.draws / r.frames), static_cast<unsigned long long>(r.distinct / r.frames), percent(r.keyRepeats), percent(r.shapeRepeats), static_cast<unsigned long long>(r.sameShapeOtherKey));
    std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
    for (const auto& [offset, count] : r.differingWords) order.emplace_back(count, offset);
    std::sort(order.rbegin(), order.rend());
    for (std::size_t i = 0; i < order.size() && i < 12; ++i) {
        const auto [old, value] = r.lastChange[order[i].second];
        std::fprintf(stderr, " 0x%03x: %llu (%08x->%08x)", order[i].second, static_cast<unsigned long long>(order[i].first), old, value);
    }
    std::fprintf(stderr, "\n");
    r.draws = r.keyRepeats = r.shapeRepeats = r.frames = r.distinct = r.sameShapeOtherKey = 0;
    r.differingWords.clear();
    r.lastChange.clear();
}

bool Driver::sameVertexInfo(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b) {
    if (a.resourcesNum != b.resourcesNum || a.fetchAttribReg != b.fetchAttribReg || a.fetchBufferReg != b.fetchBufferReg || a.fetchEmbedded != b.fetchEmbedded || a.paClVsOutCntl != b.paClVsOutCntl) return false;
    for (std::uint32_t i = 0; i < a.resourcesNum && i < a.resources.size(); ++i) {
        if (a.resources[i].fields != b.resources[i].fields) return false;
        const auto& x = a.resourcesDst[i];
        const auto& y = b.resourcesDst[i];
        if (x.registerStart != y.registerStart || x.registersNum != y.registersNum || x.attrId != y.attrId || x.fetchIndex != y.fetchIndex) return false;
    }
    return true;
}

bool Driver::sameDecode(const DrawDecode& a, const DrawDecode& b) {
    const auto& s = a.state;
    const auto& t = b.state;
    const auto sameColor = [](const Graphics::ColorTarget& x, const Graphics::ColorTarget& y) {
        return x.address == y.address && x.extent.width == y.extent.width && x.extent.height == y.extent.height && x.format == y.format && x.bytes == y.bytes && x.componentMapping == y.componentMapping && x.tileMode == y.tileMode && x.elementBytes == y.elementBytes && x.dccAddress == y.dccAddress && x.dccAlphaOnMsb == y.dccAlphaOnMsb && x.slot == y.slot && x.exportIndex == y.exportIndex;
    };
    const auto sameBlend = [](const VkPipelineColorBlendAttachmentState& x, const VkPipelineColorBlendAttachmentState& y) {
        return x.blendEnable == y.blendEnable && x.srcColorBlendFactor == y.srcColorBlendFactor && x.dstColorBlendFactor == y.dstColorBlendFactor && x.colorBlendOp == y.colorBlendOp && x.srcAlphaBlendFactor == y.srcAlphaBlendFactor && x.dstAlphaBlendFactor == y.dstAlphaBlendFactor && x.alphaBlendOp == y.alphaBlendOp && x.colorWriteMask == y.colorWriteMask;
    };
    const auto sameMesh = [](const std::optional<ShaderRecompiler::MeshConfiguration>& x, const std::optional<ShaderRecompiler::MeshConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputPrimitive == y->inputPrimitive && x->primitivesPerGroup == y->primitivesPerGroup && x->verticesPerGroup == y->verticesPerGroup && x->maxVertices == y->maxVertices && x->maxPrimitives == y->maxPrimitives && x->threadsPerGroup == y->threadsPerGroup && x->ldsSizeDwords == y->ldsSizeDwords && x->provokingVertex == y->provokingVertex && x->esgsItemSize == y->esgsItemSize;
    };
    const auto sameTess = [](const std::optional<ShaderRecompiler::TessellationConfiguration>& x, const std::optional<ShaderRecompiler::TessellationConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputControlPoints == y->inputControlPoints && x->outputControlPoints == y->outputControlPoints && x->domain == y->domain && x->partitioning == y->partitioning && x->outputTopology == y->outputTopology && x->patchesPerGroup == y->patchesPerGroup && x->primitiveGroupSize == y->primitiveGroupSize && x->breakAtInstance == y->breakAtInstance;
    };
    if (s.stages.path != t.stages.path || s.stages.registerValue != t.stages.registerValue || s.stages.vertexWaveSize != t.stages.vertexWaveSize || s.stages.fragmentWaveSize != t.stages.fragmentWaveSize || !sameMesh(s.stages.mesh, t.stages.mesh) || !sameTess(s.stages.tessellation, t.stages.tessellation)) return false;
    if (!sameColor(s.color, t.color) || s.colors.size() != t.colors.size() || s.blends.size() != t.blends.size()) return false;
    for (std::size_t i = 0; i < s.colors.size(); ++i) {
        if (!sameColor(s.colors[i], t.colors[i])) return false;
    }
    for (std::size_t i = 0; i < s.blends.size(); ++i) {
        if (!sameBlend(s.blends[i], t.blends[i])) return false;
    }
    if (s.hasColorTarget != t.hasColorTarget || s.rectList != t.rectList || s.renderExtent.width != t.renderExtent.width || s.renderExtent.height != t.renderExtent.height || s.topology != t.topology || s.negativeOneToOne != t.negativeOneToOne || s.depthClamp != t.depthClamp || s.conservativeRasterization != t.conservativeRasterization || s.cullMode != t.cullMode || s.frontFace != t.frontFace || !sameBlend(s.blend, t.blend) || s.blendConstants != t.blendConstants) return false;
    if (std::memcmp(&s.viewport, &t.viewport, sizeof(VkViewport)) != 0 || std::memcmp(&s.scissor, &t.scissor, sizeof(VkRect2D)) != 0) return false;
    const auto& p = a.pixel;
    const auto& q = b.pixel;
    if (p.interpolatorCount != q.interpolatorCount || p.interpolatorSettings != q.interpolatorSettings || p.wave32 != q.wave32 || p.inputAddr != q.inputAddr || p.hasPerspectiveCenterVgpr != q.hasPerspectiveCenterVgpr || p.perspectiveCentroid != q.perspectiveCentroid || p.posX != q.posX || p.posY != q.posY || p.posZ != q.posZ || p.posW != q.posW || p.frontFace != q.frontFace || p.ancillary != q.ancillary || p.sampleShading != q.sampleShading || p.noPerspective != q.noPerspective || p.linearCentroid != q.linearCentroid || p.pixelKillEnable != q.pixelKillEnable || p.depthExportEnable != q.depthExportEnable || p.sampleMaskExportEnable != q.sampleMaskExportEnable || p.earlyZ != q.earlyZ || p.executeOnNoop != q.executeOnNoop || p.conservativeZExport != q.conservativeZExport || p.orderedPixelShader != q.orderedPixelShader || p.stencilRefExportEnable != q.stencilRefExportEnable || p.targetOutputMode != q.targetOutputMode || p.targetExportMapping != q.targetExportMapping) return false;
    if (a.roles != b.roles || a.programs.size() != b.programs.size()) return false;
    for (std::size_t i = 0; i < a.programs.size(); ++i) {
        const auto& x = a.programs[i];
        const auto& y = b.programs[i];
        if (x.binary.stage != y.binary.stage || x.binary.codeAddress != y.binary.codeAddress || x.userDataBase != y.userDataBase || x.firstUserSgpr != y.firstUserSgpr || x.userData != y.userData || x.snapshot != y.snapshot || x.codeOffset != y.codeOffset) return false;
    }
    if (p.targetExportPacking != q.targetExportPacking || p.dualSourceBlend != q.dualSourceBlend || p.alphaToCoverage != q.alphaToCoverage || p.rasterPrimitive != q.rasterPrimitive) return false;
    return true;
}

}
