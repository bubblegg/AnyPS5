#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <cstdint>
#include <memory>

namespace ShaderRecompiler {

struct PreparedControlFlow;
struct SourceEntry;
class ShaderPreparationContext {
public:
    [[nodiscard]] std::shared_ptr<const PreparedControlFlow> AcquireFrontend(const RecompileRequest& request);
    [[nodiscard]] std::uint32_t ResourcePrograms() const;

private:
    friend IrProgram PrepareResourceProgram(const RecompileRequest& request, ShaderPreparationContext* preparation);
    friend struct PreparedInterface;
    std::shared_ptr<const PreparedControlFlow> frontend;
    std::shared_ptr<SourceEntry> interfaceSource;
    std::unique_ptr<IrProgram> interfaceProgram;
    std::uint32_t resourcePrograms = 0;
};

[[nodiscard]] IrProgram PrepareResourceProgram(const RecompileRequest& request);
[[nodiscard]] IrProgram PrepareResourceProgram(const RecompileRequest& request, ShaderPreparationContext* preparation);
[[nodiscard]] std::uint64_t FrontEndBuilds();
[[nodiscard]] std::shared_ptr<const IrResourcePlan> GetResourcePlan(const RecompileRequest& request);

struct ResourceCapture {
    std::shared_ptr<const IrResourcePlan> plan;
    ResourceSnapshot snapshot;
    // The cache entry the plan belongs to; null when the request bypasses the cache.
    std::shared_ptr<SourceEntry> source;
    // The walk's read addresses when the plan has pure flat slots (the leaf of each pure slot,
    // and every other read sorted and deduplicated); empty otherwise.
    SrtReadTrace readTrace;
    // APS5_PROFILE_DRAW: what CaptureResources spent resolving the source (the stage inputs, the
    // key over the code, the plan) before the walk.
    std::uint64_t sourceNanoseconds = 0;
};
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime);

// The resolved source of a request (its cache entry with the plan built), for a driver that
// memoizes it per registered shader: ResolveSource is what CaptureResources does before the walk
// (the stage input validation, the key over the code, the lookup, the first-sight plan build), and
// the overload below captures over a handle without repeating it. A handle stays valid for every
// request with the same code and the same cache key fields (RecompileCacheKey::ContextHash plus
// the target); the vertex stages' input validation is repeated per capture because it reads V#
// fields the key does not cover. Null for a request that bypasses the cache (useCache false).
struct CompiledVariant;
struct SourceHandle {
    std::shared_ptr<SourceEntry> source;
    std::shared_ptr<const CompiledVariant> artifact;
    std::vector<std::uint64_t> staticKey;
};
// What a caller that materializes one prepared shader again and again (the stage of a draw shape)
// keeps between its results: the binding plan and the module of the last one, with the keys they
// were found by, and its last results, whose storage serves the next once their draw is done.
// One thread at a time.
struct StageMemo;
[[nodiscard]] std::shared_ptr<StageMemo> MakeStageMemo();
// Why PreparedShaderInvocation::Recapture left a capture to the walk: the shader's plan is not one
// it is worked out for (or the capture is another shader's), or the user words differ in more
// than buffer addresses the walk passes through.
enum class RecaptureRefusal : std::uint8_t { None, Plan, Words };
class PreparedShaderInvocation {
public:
    static std::optional<PreparedShaderInvocation> TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle);
    static std::optional<PreparedShaderInvocation> TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle, std::span<const std::uint64_t> key);
    // Without TryCreate's comparisons: for a caller that knows the request has the code, the layout
    // and the interface key of a request TryCreate accepted with this handle. `validated` says the
    // vertex stage inputs are those of that request too; otherwise they are validated again.
    static PreparedShaderInvocation Reuse(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle, bool validated);
    const RecompileRequest& Request() const { return request; }
    const std::shared_ptr<const SourceHandle>& Handle() const { return handle; }
    std::shared_ptr<const ResourceCapture> Capture(const SrtRuntime& runtime) const;
    // What Capture would return for this invocation's user data, made from `previous`, an earlier
    // capture of the same prepared shader, without the walk: only where the walk provably differs
    // from the earlier one in nothing but buffer addresses taken as they are from the user data
    // (CompiledWalk::passedWords), and only as long as the guest memory the earlier walk read is
    // unchanged, which the caller checks. `previous` is taken over (and changed in place when the
    // caller is its only holder); null, with `previous` kept, when the walk has to run. `samePlan`
    // is set when the new capture also gives the binding plan the earlier one gave, for Materialize.
    std::shared_ptr<const ResourceCapture> Recapture(std::shared_ptr<const ResourceCapture>& previous, bool* samePlan = nullptr, RecaptureRefusal* refusal = nullptr) const;
    std::shared_ptr<const RecompileResult> Materialize(const ResourceCapture& capture) const;
    // The same result without the result memo's hash of the whole snapshot (APS5_VERIFY_STAGE_MEMO=N
    // compares every Nth with a result materialized without the memo).
    // `samePlan`: the capture is Recapture's of the capture the memo's last result was made from,
    // with its `samePlan` set; the binding plan is then the memo's, without its key.
    std::shared_ptr<const RecompileResult> Materialize(const ResourceCapture& capture, StageMemo* memo, bool samePlan = false) const;

private:
    PreparedShaderInvocation(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle);
    RecompileRequest request;
    std::shared_ptr<const SourceHandle> handle;
};
[[nodiscard]] std::span<const std::uint32_t> GetPreparedCode(const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request, ShaderPreparationContext* preparation);
[[nodiscard]] bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle);
void BuildPreparedShaderKey(const RecompileRequest& request, std::vector<std::uint64_t>& key);
[[nodiscard]] bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle, std::span<const std::uint64_t> key);
[[nodiscard]] const CompiledShaderArtifact& GetPreparedArtifact(const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const RecompileResult> RegistrationResult(const RecompileRequest& request, const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const RecompileResult> MaterializeShader(const RecompileRequest& request, const ResourceCapture& capture, const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const SourceHandle> ResolveSource(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime, const SourceHandle& handle);

}

#endif
