#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTCOMPILEDWALK_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTCOMPILEDWALK_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include "Optimization/SrtWalker/SrtEvaluator.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

inline constexpr std::uint32_t NoWalkNode = 0xffffffffu;

enum class WalkOp : std::uint8_t {
    Immediate,
    Fail,
    UserData,
    ShaderBase,
    Alias,
    ReadConst,
    ReadFirstLane,
    ExtractU64,
    AddCarry,
    RawRead,
    Compute
};

// One resolved IrValue of a plan, with what Evaluator::EvaluateWide finds out about it on every
// visit (Resolve, the immediate, the opcode's shape, a phi's invariant, a read's memory record)
// worked out once. Arguments are node indices in the order the evaluator takes them.
struct WalkNode {
    WalkOp op = WalkOp::Fail;
    // Compute: how many arguments the opcode takes.
    std::uint8_t arity = 0;
    // A runtime select of three arguments: under a ReadFirstLane whose mask is its first argument
    // it stands for its second.
    bool maskable = false;
    // RawRead: a ReadConstBuffer (else a LoadAddressU32), whether its handle has the four words
    // such a read needs, and LoadedFromMemory of the handle.
    bool bufferRead = false;
    bool wideHandle = false;
    bool loadedFromMemory = false;
    IrOpcode opcode = IrOpcode::Void;
    // RawRead: handle word 0, handle word 1, offset, handle word 2, handle word 3.
    // ReadFirstLane: the value, then the mask (NoWalkNode without one; compared, never evaluated).
    std::array<std::uint32_t, 5> args {NoWalkNode, NoWalkNode, NoWalkNode, NoWalkNode, NoWalkNode};
    // Immediate: the value. UserData: the word's index in the runtime's user data. ReadConst: the
    // slot. ExtractU64, AddCarry: the component. RawRead: the memory record's offset, sign-extended.
    std::uint64_t immediate = 0;
    const IrValue* inst = nullptr;
};

// The walk of one plan as nodes with dense indices, and the node of every root the walker
// evaluates. Immutable once built, so the threads that walk a shader share it.
struct CompiledWalk {
    const IrResourcePlan* plan = nullptr;
    std::vector<WalkNode> nodes;
    // By descriptor source, the first dwordCount words.
    std::vector<std::array<std::uint32_t, 8>> sources;
    // By index into srtReads, by index into controlFlow (NoWalkNode without a condition), and the
    // uniform fill's values.
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> conditions;
    std::array<std::uint32_t, 4> uniformValues {NoWalkNode, NoWalkNode, NoWalkNode, NoWalkNode};
    // What the walk does with the user data, for a caller that has the snapshot of one walk and
    // wants the snapshot of a walk with other user data (PreparedShaderInvocation::Recapture).
    // `structuralWords` (sorted) are the user words some evaluated value is computed from: a read's
    // address, a condition, a flat slot, an image or sampler word. `passedWords` are the user words
    // that are a buffer descriptor's dword as they are, and nothing else; a word that is both is
    // only listed as structural. With the structural words and the memory the walk read unchanged,
    // a walk differs from the last one in the passed words alone (of the buffers whose source is
    // active, with control flow in the plan: the conditions are structural). `replayable` is false
    // for a plan this is not worked out for (an image table, a user word beyond the plan's count).
    // `mask`: the bits of the word the snapshot keeps (a scalar-only buffer's descriptor is
    // normalized after the walk, ResourceMaterializer::ScalarOnlyBuffer).
    struct PassedWord {
        std::uint32_t buffer;
        std::uint32_t dword;
        std::uint32_t word;
        std::uint32_t mask = 0xffffffffu;
    };
    bool replayable = false;
    std::vector<std::uint32_t> structuralWords;
    std::vector<PassedWord> passedWords;
};

// The compiled walk of `plan`, which must outlive it and no longer change; null when the plan has
// something the compiler leaves to the evaluator (a malformed value, a null root), with
// APS5_NO_SRT_TAPE=1 or APS5_SRT_DEBUG, or when the opcode self-check failed.
std::shared_ptr<const CompiledWalk> CompileWalk(const IrResourcePlan& plan) noexcept;

// The runtime's compiled walk when it is the one of `plan`.
inline const CompiledWalk* UsableWalk(const SrtRuntime& runtime, const IrResourcePlan& plan) {
    return runtime.compiled != nullptr && runtime.compiled->plan == &plan ? runtime.compiled : nullptr;
}

struct CompiledWalkCounts {
    std::uint64_t compiled = 0;
    std::uint64_t refused = 0;
};
CompiledWalkCounts CompiledWalkStatistics();

// How many arguments Compute takes for an opcode; 0 for an opcode it does not compute.
std::uint8_t ComputeArity(IrOpcode opcode);
// The arithmetic of Evaluator::EvaluateInst on argument values already evaluated.
bool Compute(IrOpcode opcode, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t& result);

struct WalkValues;

// Evaluator over a compiled walk: the same values, memory reads in the same order, reports and
// failures, with a root named by its node.
class CompiledEvaluator {
public:
    CompiledEvaluator(const CompiledWalk& walk, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, CompiledEvaluator* cleanEvaluator = nullptr, std::uint32_t activeMask = NoWalkNode);
    ~CompiledEvaluator();
    CompiledEvaluator(const CompiledEvaluator&) = delete;
    CompiledEvaluator& operator=(const CompiledEvaluator&) = delete;

    bool Evaluate(std::uint32_t node, std::uint32_t& result);
    bool EvaluateWide(std::uint32_t node, std::uint64_t& result);
    void ReportInaccessibleReads(InaccessibleRead* sink) { _inaccessible = sink; }
    void ReportNullRootReads(std::uint32_t* sink) { _nullRoots = sink; }

private:
    bool Run(const WalkNode& node, std::uint64_t& result);
    bool RunRawRead(const WalkNode& node, std::uint64_t& result);

    const CompiledWalk& _walk;
    const WalkNode* _nodes;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    CompiledEvaluator* _cleanEvaluator = nullptr;
    std::uint32_t _activeMask = NoWalkNode;
    InaccessibleRead* _inaccessible = nullptr;
    std::uint32_t* _nullRoots = nullptr;
    WalkValues* _values;
    std::uint32_t _generation = 0;
};

}

#endif
