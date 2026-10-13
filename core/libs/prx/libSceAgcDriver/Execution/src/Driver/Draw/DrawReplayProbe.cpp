#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace AgcDriver::DriverDetail {

// Measuring aid for a replayed draw, APS5_REPLAY_PROBE=1: every draw of a shape whose record this
// draw holds is compared with the shape's previous draw, stage by stage, and the outcomes are
// printed every 10 s as [replay-probe] lines. Nothing it finds changes the draw.
struct ReplayProbeStage {
    bool valid = false;
    std::vector<std::uint32_t> userData;
    // Bit i: user word i differed from the previous draw's at least once.
    std::uint64_t everDiffered = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::vector<std::uint32_t> words;
    std::vector<ShaderRecompiler::DescriptorBinding> bindings;
    std::vector<std::byte> push;
    std::vector<std::array<std::uint32_t, 4>> attributes;
    std::uint64_t variant = 0;
};

struct ReplayProbeDraw {
    bool valid = false;
    std::uint64_t indexAddress = 0;
    std::uint32_t indexCount = 0, instanceCount = 0;
    std::uint64_t colorAddress = 0, depthAddress = 0;
};

bool ReplayProbeEnabled() {
    static const bool enabled = std::getenv("APS5_REPLAY_PROBE") != nullptr;
    return enabled;
}

namespace {

enum Walk { WalkSame, WalkSameAddresses, WalkMoved, WalkCount };
enum Bind { BindSame, BindBases, BindData, BindBasesData, BindBufferOther, BindTextures, BindLayout, BindVariant, BindCount };
enum Attr { AttrSame, AttrBases, AttrOther, AttrCount };

struct StageTotals {
    std::uint64_t stages = 0, userSame = 0, userKnown = 0, pushSame = 0, basesTotal = 0, basesFromUser = 0, walkWordsDiffering = 0;
    std::array<std::uint64_t, 5> userDiffering{};
    std::array<std::uint64_t, WalkCount> walk{};
    std::array<std::uint64_t, BindCount> bind{};
    std::array<std::uint64_t, AttrCount> attr{};
};

struct Totals {
    std::mutex mutex;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    std::uint64_t draws = 0, first = 0, replayable = 0, replayableUserBases = 0, indexSame = 0, indexCountSame = 0, instancesSame = 0, targetsSame = 0;
    std::array<StageTotals, 2> stages;
};

Totals& totals() {
    static Totals instance;
    return instance;
}

bool sameBufferFieldsBut(const std::uint32_t* a, const std::uint32_t* b) {
    return (a[1] & 0xffff0000u) == (b[1] & 0xffff0000u) && a[2] == b[2] && a[3] == b[3];
}

bool dataRole(ShaderRecompiler::DescriptorRole role) {
    return role == ShaderRecompiler::DescriptorRole::ShaderData || role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
}

const char* percent(char* text, std::uint64_t part, std::uint64_t whole) {
    std::snprintf(text, 16, "%.1f%%", whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0);
    return text;
}

void report(Totals& t) {
    const auto now = std::chrono::steady_clock::now();
    if (now - t.since < std::chrono::seconds(10)) return;
    const double seconds = std::chrono::duration<double>(now - t.since).count();
    char a[16], b[16], c[16], d[16], e[16], f[16], g[16], h[16], k[16];
    std::fprintf(stderr, "[replay-probe] %.1f s: %llu draws compared with their shape's previous one (%llu first of their shape); replayable %s (with every changed buffer address found in the user words %s); index buffer same %s, index count %s, instances %s, targets %s\n", seconds, static_cast<unsigned long long>(t.draws), static_cast<unsigned long long>(t.first), percent(a, t.replayable, t.draws), percent(b, t.replayableUserBases, t.draws), percent(c, t.indexSame, t.draws), percent(d, t.indexCountSame, t.draws), percent(e, t.instancesSame, t.draws), percent(f, t.targetsSame, t.draws));
    for (std::size_t kind = 0; kind < t.stages.size(); ++kind) {
        auto& s = t.stages[kind];
        const auto n = s.stages;
        std::fprintf(stderr, "[replay-probe]   %s stages %llu: user words same %s, differing in 1 / 2 / 3-4 / 5-8 / 9+ words %llu / %llu / %llu / %llu / %llu, only in words that differed before %s; walk reads same %s, same addresses other words %s (%.1f words each), elsewhere %s\n", kind == 0 ? "vertex" : "pixel", static_cast<unsigned long long>(n), percent(a, s.userSame, n), static_cast<unsigned long long>(s.userDiffering[0]), static_cast<unsigned long long>(s.userDiffering[1]), static_cast<unsigned long long>(s.userDiffering[2]), static_cast<unsigned long long>(s.userDiffering[3]), static_cast<unsigned long long>(s.userDiffering[4]), percent(b, s.userKnown, n), percent(c, s.walk[WalkSame], n), percent(d, s.walk[WalkSameAddresses], n), s.walk[WalkSameAddresses] != 0 ? static_cast<double>(s.walkWordsDiffering) / static_cast<double>(s.walk[WalkSameAddresses]) : 0.0, percent(e, s.walk[WalkMoved], n));
        std::fprintf(stderr, "[replay-probe]   %s descriptors: same %s, buffer addresses only %s, shader data only %s, both %s, other buffer fields %s, textures or samplers %s, binding layout %s, another variant %s; changed buffer addresses found in the user words %llu of %llu; push constants same %s; vertex buffers same %s, addresses only %s, other %s\n", kind == 0 ? "vertex" : "pixel", percent(a, s.bind[BindSame], n), percent(b, s.bind[BindBases], n), percent(c, s.bind[BindData], n), percent(d, s.bind[BindBasesData], n), percent(e, s.bind[BindBufferOther], n), percent(f, s.bind[BindTextures], n), percent(g, s.bind[BindLayout], n), percent(h, s.bind[BindVariant], n), static_cast<unsigned long long>(s.basesFromUser), static_cast<unsigned long long>(s.basesTotal), percent(k, s.pushSame, n), percent(a, s.attr[AttrSame], n), percent(b, s.attr[AttrBases], n), percent(c, s.attr[AttrOther], n));
    }
    t.draws = t.first = t.replayable = t.replayableUserBases = t.indexSame = t.indexCountSame = t.instancesSame = t.targetsSame = 0;
    t.stages = {};
    t.since = now;
}

}

void ProbeReplay(ShapeStages& shape, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<StageCapture>& captures, std::span<const ShaderRecompiler::RecompileResult* const> results, const Pm4::DrawParameters& draw, const Graphics::State& state) {
    using Role = ShaderRecompiler::DescriptorRole;
    if (shape.stages.size() != programs.size() || results.size() != programs.size()) return;
    auto& t = totals();
    std::lock_guard lock(t.mutex);
    if (shape.probe == nullptr) shape.probe = std::make_shared<ReplayProbeDraw>();
    auto& drawRecord = *shape.probe;
    const auto depthAddress = state.depth ? state.depth->address : 0u;
    bool compared = drawRecord.valid;
    bool replayable = true, userBases = true;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto* result = results[i];
        if (result == nullptr || roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        auto& recall = shape.stages[i];
        if (recall.probe == nullptr) recall.probe = std::make_shared<ReplayProbeStage>();
        auto& record = *recall.probe;
        const auto& user = programs[i].userData;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::uint32_t> words;
        for (const auto& region : captures[i].regions) {
            runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            const auto offset = words.size();
            words.resize(offset + count);
            std::memcpy(words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        }
        if (record.valid && compared) {
            auto& s = t.stages[programs[i].binary.stage == ShaderRecompiler::ShaderStage::Fragment ? 1 : 0];
            ++s.stages;
            std::uint64_t differing = 0;
            std::size_t count = 0;
            if (record.userData.size() != user.size()) {
                count = 64;
            } else {
                for (std::size_t w = 0; w < user.size(); ++w) {
                    if (user[w] == record.userData[w]) continue;
                    ++count;
                    if (w < 64) differing |= std::uint64_t{1} << w;
                }
            }
            if (count == 0) ++s.userSame;
            else ++s.userDiffering[count == 1 ? 0 : count == 2 ? 1 : count <= 4 ? 2 : count <= 8 ? 3 : 4];
            if (count != 0 && (differing & ~record.everDiffered) == 0 && count < 64) ++s.userKnown;
            record.everDiffered |= differing;
            if (runs != record.runs) {
                ++s.walk[WalkMoved];
            } else if (words != record.words) {
                ++s.walk[WalkSameAddresses];
                for (std::size_t w = 0; w < words.size(); ++w) s.walkWordsDiffering += words[w] != record.words[w];
            } else {
                ++s.walk[WalkSame];
            }
            Bind bind = BindSame;
            if (result->PipelineVariantId() != record.variant) {
                bind = BindVariant;
            } else if (result->bindings.size() != record.bindings.size()) {
                bind = BindLayout;
            } else {
                bool bases = false, data = false, bufferOther = false, textures = false, layout = false;
                for (std::size_t b = 0; b < result->bindings.size(); ++b) {
                    const auto& now = result->bindings[b];
                    const auto& was = record.bindings[b];
                    if (now.role != was.role || now.count != was.count || now.binding != was.binding || now.guestDescriptor.size() != was.guestDescriptor.size()) {
                        layout = true;
                        continue;
                    }
                    if (now.guestDescriptor == was.guestDescriptor) continue;
                    if (dataRole(now.role)) {
                        data = true;
                    } else if (now.role == Role::GuestBuffers && now.guestDescriptor.size() == static_cast<std::size_t>(now.count) * 4u) {
                        for (std::uint32_t element = 0; element < now.count; ++element) {
                            const auto* x = now.guestDescriptor.data() + element * 4u;
                            const auto* y = was.guestDescriptor.data() + element * 4u;
                            if (std::memcmp(x, y, 16) == 0) continue;
                            if (!sameBufferFieldsBut(x, y)) {
                                bufferOther = true;
                                continue;
                            }
                            bases = true;
                            ++s.basesTotal;
                            if (std::find(user.begin(), user.end(), x[0]) != user.end()) ++s.basesFromUser;
                            else userBases = false;
                        }
                    } else if (now.role == Role::GuestImages || now.role == Role::GuestSamplers) {
                        textures = true;
                    } else {
                        bufferOther = true;
                    }
                }
                bind = layout ? BindLayout : textures ? BindTextures : bufferOther ? BindBufferOther : bases && data ? BindBasesData : bases ? BindBases : data ? BindData : BindSame;
            }
            ++s.bind[bind];
            if (bind != BindSame && bind != BindBases && bind != BindData && bind != BindBasesData) replayable = false;
            if (result->pushConstants.size() == record.push.size() && std::equal(result->pushConstants.begin(), result->pushConstants.end(), record.push.begin())) ++s.pushSame;
            Attr attr = AttrSame;
            if (result->vertexAttributes.size() != record.attributes.size()) {
                attr = AttrOther;
            } else {
                for (std::size_t v = 0; v < record.attributes.size(); ++v) {
                    const auto& x = result->vertexAttributes[v].resource.fields;
                    const auto& y = record.attributes[v];
                    if (std::equal(x.begin(), x.end(), y.begin())) continue;
                    if (sameBufferFieldsBut(x.data(), y.data())) attr = std::max(attr, AttrBases);
                    else attr = AttrOther;
                }
            }
            ++s.attr[attr];
            if (attr == AttrOther) replayable = false;
        } else {
            compared = false;
        }
        record.valid = true;
        record.userData.assign(user.begin(), user.end());
        record.runs = std::move(runs);
        record.words = std::move(words);
        record.bindings = result->bindings;
        record.push.assign(result->pushConstants.begin(), result->pushConstants.end());
        record.attributes.clear();
        for (const auto& attribute : result->vertexAttributes) record.attributes.push_back(attribute.resource.fields);
        record.variant = result->PipelineVariantId();
    }
    if (compared) {
        ++t.draws;
        if (replayable) ++t.replayable;
        if (replayable && userBases) ++t.replayableUserBases;
        if (draw.indexAddress == drawRecord.indexAddress) ++t.indexSame;
        if (draw.indexCount == drawRecord.indexCount) ++t.indexCountSame;
        if (draw.instanceCount == drawRecord.instanceCount) ++t.instancesSame;
        if ((state.hasColorTarget ? state.color.address : 0u) == drawRecord.colorAddress && depthAddress == drawRecord.depthAddress) ++t.targetsSame;
    } else {
        ++t.first;
    }
    drawRecord.valid = true;
    drawRecord.indexAddress = draw.indexAddress;
    drawRecord.indexCount = draw.indexCount;
    drawRecord.instanceCount = draw.instanceCount;
    drawRecord.colorAddress = state.hasColorTarget ? state.color.address : 0u;
    drawRecord.depthAddress = depthAddress;
    report(t);
}

}
