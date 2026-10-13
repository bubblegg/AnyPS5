#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace ShaderRecompiler {
struct SourceHandle;
struct StageMemo;
struct ResourceCapture;
}

namespace AgcDriver::DriverDetail {

struct DrawProgram {
    ShaderRecompiler::ShaderBinary binary;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr = 8;
    std::vector<std::uint32_t> userData;
    std::array<ShaderRecompiler::MemoryRegion, 2> memory;

    std::shared_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
};

struct DrawDecode {
    Graphics::State state;
    ShaderRecompiler::ShaderPixelStageInfo pixel;
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::ProgramRole> roles;
};

void DecodeGraphicsPrograms(DrawDecode& decoded, const QueueState& queue, const ShaderRegistry& registry, bool staticAbi, bool includeFragment);

struct PreparedGraphicsStage {
    std::shared_ptr<const ShaderSnapshot> snapshot;
    PreparedShaders::Entry entry;
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    std::uint32_t pushOffset = 0;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> registration;
};

std::vector<PreparedGraphicsStage> PrepareGraphicsStages(const DrawDecode& decoded, const ShaderRecompiler::SpirvTarget& target);

struct DrawRecipeRecord {
    std::vector<std::weak_ptr<const DispatchVariant>> stages;
    std::shared_ptr<const DrawRecipe> recipe;
    bool Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const;
    bool Expired() const;
};

struct DrawEntry {

    std::shared_ptr<const DrawDecode> decode;

    std::vector<std::vector<std::shared_ptr<DispatchVariant>>> stages;

    std::atomic<std::shared_ptr<const std::vector<DrawRecipeRecord>>> recipes;
    std::uint64_t touched = 0;
    std::list<std::uint64_t>::iterator order;
};

enum class DrawMiss : std::size_t { FrontDiffering, FragmentDiffering, OtherDiffering, Layout, Gate, Stages, Count };

struct DrawEntryCounters {
    std::uint64_t lookups = 0, absent = 0, hits = 0, stageValidations = 0, stageEqual = 0, variantsCompared = 0, inserts = 0, variantsInserted = 0, variantsEvicted = 0, present = 0, unstable = 0, touches = 0, verifyHits = 0, verifyMismatches = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> misses{};
    std::array<std::uint64_t, MaxDispatchVariants> variantHitsByRank{};
    double validateUs = 0;

    std::uint64_t registerKeyLookups = 0, registerKeyHits = 0, decodeSkipped = 0, decodePartial = 0, facadeMismatches = 0, verifyDecodes = 0, verifyDecodeMismatches = 0;
    std::uint64_t shapeDecodeHits = 0, shapeDecodeMisses = 0, shapeDecodeMismatches = 0, insertsSkipped = 0;
    double keyUs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

enum class DrawVerdict { Drawn, Nothing, Rejected };

struct StageCapture {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    std::uint64_t forgetSerial = 0;
    std::uint32_t pushOffset = 0;
};

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry& registry);

struct ReplayProbeStage;
struct ReplayProbeDraw;

// What a stage of a shape's last draw resolved to, and what it was resolved from: a draw of the
// shape that gives the stage the same takes the prepared artifact from here (APS5_NO_STAGE_MEMO=1
// resolves every stage as before; APS5_VERIFY_STAGE_MEMO=N resolves every Nth as well and compares).
struct ShapeStage {
    std::shared_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    std::uint32_t waveSize = 0, firstUserSgpr = 0, userWords = 0, pushOffset = 0, pushSize = 0;
    // A fragment stage: the variant whose exports defaulted its unexported inputs (0: none).
    std::uint64_t pixelFrom = 0;
    std::unique_ptr<ShaderRecompiler::ShaderVertexStageInfo> vertex;
    std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    std::shared_ptr<ShaderRecompiler::StageMemo> memo;
    // Stage replay (fragment stages): the result last materialized for the stage, the user words it
    // was captured with and the dwords its capture read. A draw that gives the stage the same
    // prepared shader and user words, with those dwords unchanged, takes the result without the
    // capture and the materialization (APS5_NO_STAGE_REPLAY=1 captures every stage as before;
    // APS5_VERIFY_STAGE_REPLAY=N captures every Nth replayed stage as well and compares).
    std::shared_ptr<const ShaderRecompiler::RecompileResult> replayResult;
    std::vector<std::uint32_t> replayUser;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> replayRuns;
    std::vector<std::uint32_t> replayWords;
    // Capture replay (every stage): the stage's last capture, whose walk read `replayRuns`. A draw
    // that differs from it only in buffer addresses the walk passes through from the user words
    // takes it with those changed instead of walking (APS5_NO_CAPTURE_REPLAY=1 walks as before).
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> replayCapture;
    // The replayed result's part of the draw's resource cache key (Graphics::DrawStageContentKey),
    // built once for the result it names and shared with the draws that replay it.
    std::shared_ptr<const std::vector<std::uint32_t>> replayKey;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> replayKeyOf;
    // The stage's last resource cache key part and the binding plan and variant of the result it
    // was built from: a draw whose part is the same (a vertex stage's usually is: only its buffer
    // addresses move, and they are not in the key) shares it instead of building its own.
    std::shared_ptr<const std::vector<std::uint32_t>> lastKey;
    const void* lastKeyPlan = nullptr;
    std::uint64_t lastKeyVariant = 0;
    ShaderRecompiler::ShaderStage lastKeyStage = ShaderRecompiler::ShaderStage::Vertex;
    // APS5_REPLAY_PROBE: the stage of the shape's previous draw (DrawReplayProbe.cpp).
    std::shared_ptr<ReplayProbeStage> probe;
};

// Held by one draw at a time (`busy`); a draw that finds it taken resolves its stages as before.
struct ShapeStages {
    std::atomic_flag busy;
    std::vector<ShapeStage> stages;
    // The recording side's memo of the shape; under the GPU mutex, not under `busy`.
    Graphics::DrawShapeMemo drawn;
    std::shared_ptr<ReplayProbeDraw> probe;
};

// Measuring aid, APS5_REPLAY_PROBE=1: compares a draw of a shape with the shape's previous draw
// (user words, the walk's reads, descriptors, vertex buffers, draw parameters) and prints the
// outcomes every 10 s as [replay-probe] lines. Called with the shape's record held.
bool ReplayProbeEnabled();
void ProbeReplay(ShapeStages& shape, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<StageCapture>& captures, std::span<const ShaderRecompiler::RecompileResult* const> results, const Pm4::DrawParameters& draw, const Graphics::State& state);

struct StageMemoCounts {
    std::atomic<std::uint64_t> reused{0}, revalidated{0}, resolved{0};
    // Stage replay: results taken from the shape, and fragment stages that could not (other user
    // words; the same words over changed memory).
    std::atomic<std::uint64_t> replayed{0}, replayOtherWords{0}, replayOtherMemory{0};
    // Capture replay: stages (of any kind) whose capture was taken from the shape without a walk.
    std::atomic<std::uint64_t> recaptured{0};
};
StageMemoCounts& StageMemoCounters();

}

#endif
