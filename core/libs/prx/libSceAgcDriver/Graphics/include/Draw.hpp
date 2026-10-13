#include <string>
#include <optional>
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

struct HostImport;

// What the last draw of a draw shape (the driver's: one register state apart from the user data
// words) derived from its stages, its vertex attributes and its state alone, for the next draw of
// the shape to take when those have the same signature: the fragment outputs of the stage
// validation, the vertex input layout and the pipeline. Every draw built a validation key and a
// pipeline key and looked both up, and built and checked its vertex input layout anew. A draw that
// differs is derived as before and remembered; so is every 64th, which keeps the pipeline store's
// order of use current. Used under the GPU mutex only. APS5_NO_DRAW_SHAPE_MEMO=1 derives every
// draw as before; APS5_VERIFY_DRAW_SHAPE_MEMO=N derives every Nth the long way too and compares.
struct DrawShapeMemo {
    struct Attribute {
        std::uint32_t location = 0, components = 0, formatComponents = 0, fetchIndex = 0;
        // The V#'s format word and its stride with the flag above it; the format's alignment.
        std::uint32_t format = 0, stride = 0, alignment = 1;
        bool null = false;
    };
    VkDevice device = VK_NULL_HANDLE;
    std::weak_ptr<BufferPool> pool;
    std::uint32_t uses = 0;
    // This draw: derived the long way to refresh the memo, or also the long way to compare.
    bool refresh = false, verify = false;
    // Counts the changes of what the pipeline was looked up with.
    std::uint64_t generation = 0;
    bool outputsValid = false;
    std::vector<std::uint64_t> validationKey;
    std::set<std::uint32_t> fragmentOutputs;
    VkPipelineStageFlags shaderStages = 0;
    bool layoutValid = false;
    std::vector<Attribute> attributes;
    VertexInputLayout vertexInput;
    std::uint64_t pipelineGeneration = ~std::uint64_t{0};
    std::weak_ptr<Pipeline> pipeline;
    VkDescriptorSetLayout mainLayout = VK_NULL_HANDLE, fragmentLayout = VK_NULL_HANDLE;
    bool split = false;
    std::vector<std::uint32_t> mainKey, fragmentKey;
    VkImageLayout attachmentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t proxiedTargets = 0;
    // The resource cache key of the shape's last draw and the template the cache served it with:
    // a draw whose key is the same (compared word by word, not built and hashed) takes the
    // template while the cache still holds it, and proves it current as after a lookup.
    ResourceCache::Key resourceKey;
    std::weak_ptr<ShaderResources> resources;
    // Per stage, the binding plan, variant and stage of a shader whose part of `resourceKey` was
    // proved equal (or that the key was built from): a stage with the same three compares only its
    // guest descriptors against the key (ShaderResources::ContentKeyIs's `fixedKnown`).
    struct KeyStamp {
        const void* plan = nullptr;
        std::uint64_t variant = 0;
        std::uint32_t stage = 0;
    };
    std::vector<KeyStamp> keyStamps;
    // Earlier keys of the shape with their templates and stamps, most recent first (draws of a shape
    // alternate between materials): a draw whose key is one of them swaps it with the one above
    // instead of building and hashing its key. APS5_NO_SPARE_RESOURCE_KEYS=1 keeps one key only.
    struct SpareResources {
        ResourceCache::Key key;
        std::weak_ptr<ShaderResources> resources;
        std::vector<KeyStamp> stamps;
    };
    std::array<SpareResources, 3> spareResources;
    // The resident image of each color attachment of the shape's last draw and the target it was
    // looked up for: a draw with the same target takes the image while the storage cache still holds
    // it for its surface, and refreshes it as the lookup would (as DrawWithRecipe does).
    std::vector<std::weak_ptr<StorageTexture>> targets;
    std::vector<ColorTarget> targetColors;
};

struct DrawShapeMemoCounts {
    std::atomic<std::uint64_t> draws{0}, outputs{0}, layouts{0}, pipelines{0}, resources{0}, targets{0};
};
DrawShapeMemoCounts& DrawShapeMemoCounters();

// `recipe`, when given, receives the DrawRecipe a recorded, cacheable, reusable, non-indirect draw
// built for its draw-cache entry (design_cpu_final M8; null otherwise, and always under
// APS5_NO_DRAW_RECIPE=1).
void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr, DrawShapeMemo* memo = nullptr);
std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state);
std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews);

struct DrawInputCopy {
    std::shared_ptr<Buffer> buffer;
    bool reused = false;
    std::uint32_t derived = 0;
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
};
DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use);
DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes);
void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived);
const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment);
struct InPlaceIndices {
    std::optional<std::uint32_t> highest;
    bool reused = false;
};
InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart);

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw);

struct MeshArguments {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
};
static_assert(sizeof(MeshArguments) == ShaderRecompiler::MeshArgumentBytes);
struct MeshArgumentRules {
    std::uint32_t indexCount;
    std::uint32_t inputSize;
    std::uint32_t step;
    std::uint32_t primitivesPerGroup;
    std::uint32_t maxGroups;
    std::uint32_t maxInstances;
    std::uint32_t maxTotal;
};
MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount);
MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules);

// Why DrawWithRecipe did not record from the recipe (the caller then runs Draw): the draw is not
// recordable (no recorder, APS5_SYNC_DRAWS, APS5_DUMP_TARGETS), a resident target is gone from the
// storage cache, the template is gone, the pipeline is gone from the pipeline store, or the
// template's proof failed (ShaderResources::ProveCurrent; the template is removed from the
// resource cache and kept by the batch).
enum class DrawRecipeMiss : std::uint8_t { None, NotRecordable, TargetGone, TemplateGone, ObjectsGone, Proof, Count };
const char* DrawRecipeMissName(DrawRecipeMiss miss);
struct DrawRecipeOutcome {
    bool recorded = false;
    DrawRecipeMiss miss = DrawRecipeMiss::None;
    ShaderResources::ProofReport proof;
    // APS5_PROFILE_DRAW: the proof's and the record's time.
    double proofUs = 0;
    double recordUs = 0;
};
// A draw-cache hit recorded from its recipe, under GuestMemory::GpuMutex after the packet's labels
// (design_cpu_final M8, step 8b): the validation with the recipe's stored outputs, the index and
// vertex work as Draw's, the resident-target proof on the stored images (Refresh while Cached()),
// the template's proof (ProveCurrent) with the alias checks repeated, the pipeline and framebuffer
// from the recipe, then the record as Draw's (pass continued or begun, Kept, MarkGpuWrites). A
// miss records nothing.
DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe);
// Whether recorded draws build recipes and draw-cache hits use them (APS5_NO_DRAW_RECIPE unset).
bool DrawRecipes();

// How an indirect draw's records were read, for the [draws] line (APS5_PROFILE_DRAW): Gpu is
// vkCmdDrawIndirect from the host import; every other value names why the CPU read them instead,
// either decided by the driver before the draw (its patched-SGPR fold rules, the shader path, the
// draw index, the vertex ranges, the device features, APS5_NO_GPU_INDIRECT_DRAW) or under the device
// lock in Draw (the memory state of the records). `readMs` is the CPU read's time (its sync);
// `rewritten` counts a GPU-side draw whose records were copied and patched with a constant.
enum class IndirectDrawPath : std::uint8_t { Gpu, NotFolded, FetchUnknown, NonVertexPath, IndxOffset, DrawIndex, VertexRange, FeatureGap, PendingImage, PendingLabelOrCopy, NotImported, Disabled, Count };
void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten = false);
const char* IndirectDrawPathName(IndirectDrawPath path);

// Draw packets that drew nothing, for the [draws] line: Nothing (an empty count, or no color writes
// and no pixel shader), Prechecked (State.hpp's DrawRejection refused it before the decode) and
// Thrown (the decode or the build threw); `us` is the packet's time in the driver.
enum class DrawSkip : std::uint8_t { Nothing, Prechecked, Thrown, Count };
void CountDrawSkip(DrawSkip kind, double us);

// Recorded draws whose written guest buffers were copied (their results reach guest memory by a CPU
// write-back when their batch completes), listed until that write-back ran. It is the draw
// counterpart of VulkanDevice's State::copiedWriters for dispatches, which VulkanDevice.cpp consults
// in two places: DispatchIndirect (an indirect dispatch whose arguments such a writer produced reads
// them on the CPU, behind the write-back, instead of from the host import on the GPU) and the fill
// HLE (a vkCmdFillBuffer over a range a listed write-back will land on must wait for it, or the
// write-back overwrites the fill). Draws are listed here only under APS5_RECORD_COPIED_DRAWS=1,
// which is safe only once BOTH of those checks also test this list, i.e. each extends its
// `std::any_of(state->copiedWriters ...)` with
// `|| std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(<its range>); })`;
// cleaner still, State::copiedWriters can become this list (one registry for both producers), after
// which the switch can turn into APS5_NO_RECORD_COPIED_DRAWS. Until then such draws are recorded and
// waited for at once. Read and written under GuestMemory::GpuMutex, like the device's list.
std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters();

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass);

}

#endif
