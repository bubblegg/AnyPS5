#include "Optimization/SrtWalker/SrtCompiledWalk.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ShaderRecompiler::Detail {

// A walk's values and "being evaluated" marks by node, reused by a thread's evaluators as
// ValueTable is: a slot belongs to the current walk when its stamp is the table's generation.
struct WalkValues {
    struct Slot {
        std::uint64_t value = 0;
        std::uint32_t stamp = 0;
        std::uint32_t visiting = 0;
    };
    std::vector<Slot> slots;
    std::uint32_t generation = 0;
};

namespace {

std::vector<std::unique_ptr<WalkValues>>& spareWalkValues() {
    thread_local std::vector<std::unique_ptr<WalkValues>> spare;
    return spare;
}

WalkValues* acquireWalkValues(std::size_t nodes) {
    auto& spare = spareWalkValues();
    WalkValues* values = nullptr;
    if (spare.empty()) {
        values = new WalkValues();
    } else {
        values = spare.back().release();
        spare.pop_back();
    }
    try {
        if (values->slots.size() < nodes) values->slots.resize(nodes);
    } catch (...) {
        delete values;
        throw;
    }
    if (++values->generation == 0) {
        for (auto& slot : values->slots) slot = {};
        values->generation = 1;
    }
    return values;
}

void releaseWalkValues(WalkValues* values) noexcept {
    try {
        spareWalkValues().emplace_back(values);
    } catch (...) {
        delete values;
    }
}

float float32(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }

std::uint64_t float32Bits(float value) { return std::bit_cast<std::uint32_t>(value); }

std::atomic<std::uint64_t> compiledWalks {0};
std::atomic<std::uint64_t> refusedWalks {0};

class WalkCompiler {
public:
    WalkCompiler(const IrResourcePlan& plan, CompiledWalk& walk) : _plan(plan), _walk(walk) {}

    void Run() {
        _walk.plan = &_plan;
        _walk.sources.resize(_plan.descriptorSources.size());
        for (std::size_t source = 0; source < _plan.descriptorSources.size(); ++source) {
            const auto& from = _plan.descriptorSources[source];
            auto& to = _walk.sources[source];
            to.fill(NoWalkNode);
            if (from.dwordCount > from.dwords.size()) throw std::runtime_error("descriptor source is wider than its words");
            for (std::uint32_t index = 0; index < from.dwordCount; ++index) to[index] = Node(from.dwords[index]);
        }
        _walk.reads.reserve(_plan.srtReads.size());
        for (const auto& read : _plan.srtReads) _walk.reads.push_back(Node(read.value));
        _walk.conditions.reserve(_plan.controlFlow.size());
        for (const auto& block : _plan.controlFlow) _walk.conditions.push_back(block.condition != nullptr ? Node(block.condition) : NoWalkNode);
        if (_plan.uniformFill.fill.kind != UniformFillKind::None) {
            const auto words = std::min<std::size_t>(_plan.uniformFill.fill.words, _plan.uniformFill.values.size());
            for (std::size_t index = 0; index < words; ++index) _walk.uniformValues[index] = Node(_plan.uniformFill.values[index]);
        }
        ClassifyUserWords();
    }

private:
    // See CompiledWalk::structuralWords. A buffer's dword is passed when its node is a user word
    // through nothing but aliases; every other root, and everything it reaches, is structural.
    void ClassifyUserWords() {
        if (_plan.info.buffers.size() > _plan.materializationSources.size()) return;
        for (const auto& source : _plan.descriptorSources) {
            if (source.indirectImage.has_value()) return;
        }
        std::vector<std::uint8_t> passedSource(_plan.descriptorSources.size(), 0u);
        for (std::size_t buffer = 0; buffer < _plan.info.buffers.size(); ++buffer) passedSource.at(_plan.materializationSources[buffer]) = 1u;
        std::vector<std::uint8_t> reached(_walk.nodes.size(), 0u);
        std::vector<std::uint32_t> pending;
        const auto reach = [&](std::uint32_t node) {
            if (node == NoWalkNode || reached[node] != 0u) return;
            reached[node] = 1u;
            pending.push_back(node);
        };
        const auto passed = [&](std::uint32_t node) {
            while (node != NoWalkNode && _walk.nodes[node].op == WalkOp::Alias) node = _walk.nodes[node].args[0];
            return node != NoWalkNode && _walk.nodes[node].op == WalkOp::UserData ? node : NoWalkNode;
        };
        for (std::size_t source = 0; source < _walk.sources.size(); ++source) {
            for (const auto node : _walk.sources[source]) {
                if (passedSource[source] == 0u || passed(node) == NoWalkNode) reach(node);
            }
        }
        for (const auto node : _walk.reads) reach(node);
        for (const auto node : _walk.conditions) reach(node);
        for (const auto node : _walk.uniformValues) reach(node);
        while (!pending.empty()) {
            const auto& node = _walk.nodes[pending.back()];
            pending.pop_back();
            for (const auto argument : node.args) reach(argument);
        }
        for (std::size_t index = 0; index < _walk.nodes.size(); ++index) {
            const auto& node = _walk.nodes[index];
            if (node.op != WalkOp::UserData) continue;
            if (node.immediate >= _plan.userDataCount) return;
            if (reached[index] != 0u) _walk.structuralWords.push_back(static_cast<std::uint32_t>(node.immediate));
        }
        std::sort(_walk.structuralWords.begin(), _walk.structuralWords.end());
        _walk.structuralWords.erase(std::unique(_walk.structuralWords.begin(), _walk.structuralWords.end()), _walk.structuralWords.end());
        for (std::size_t buffer = 0; buffer < _plan.info.buffers.size(); ++buffer) {
            const auto& dwords = _walk.sources[_plan.materializationSources[buffer]];
            for (std::uint32_t dword = 0; dword < dwords.size(); ++dword) {
                const auto node = passed(dwords[dword]);
                if (node == NoWalkNode) continue;
                const auto word = static_cast<std::uint32_t>(_walk.nodes[node].immediate);
                if (!std::binary_search(_walk.structuralWords.begin(), _walk.structuralWords.end(), word)) _walk.passedWords.push_back({static_cast<std::uint32_t>(buffer), dword, word});
            }
        }
        _walk.replayable = true;
    }

    static IrValue* Resolved(const IrValue& inst, std::size_t index) {
        IrValue* argument = inst.Argument(index);
        if (argument == nullptr) throw std::runtime_error("null argument");
        return argument->Resolve();
    }

    std::uint32_t Slot(IrValue* value) {
        const auto [found, inserted] = _index.try_emplace(value, static_cast<std::uint32_t>(_walk.nodes.size()));
        if (inserted) {
            _walk.nodes.emplace_back();
            _built.push_back(0u);
        }
        return found->second;
    }

    // The node of a value that is only compared with, never evaluated through this edge.
    std::uint32_t Reference(IrValue* raw) {
        return Slot(raw->Resolve());
    }

    std::uint32_t Node(IrValue* raw) {
        if (raw == nullptr) throw std::runtime_error("null value");
        IrValue* value = raw->Resolve();
        const auto index = Slot(value);
        if (_built[index] != 0u) return index;
        _built[index] = 1u;
        const WalkNode node = Build(*value);
        _walk.nodes[index] = node;
        return index;
    }

    WalkNode Build(IrValue& value) {
        WalkNode node;
        node.inst = &value;
        node.opcode = value.Opcode();
        if (value.HasImmediate()) {
            node.op = WalkOp::Immediate;
            switch (value.Type()) {
                case IrType::Bool: node.immediate = value.ImmediateBool() ? 1u : 0u; break;
                case IrType::U8: node.immediate = value.ImmediateU8(); break;
                case IrType::U16: node.immediate = value.ImmediateU16(); break;
                case IrType::U32: node.immediate = value.ImmediateU32(); break;
                case IrType::U64: node.immediate = value.ImmediateU64(); break;
                case IrType::F32: node.immediate = float32Bits(value.ImmediateF32()); break;
                default: node.op = WalkOp::Fail; break;
            }
            return node;
        }
        switch (value.Opcode()) {
            case IrOpcode::Void: return node;
            case IrOpcode::GetUserData: {
                IrValue* source = value.Argument(0);
                if (source == nullptr) throw std::runtime_error("null argument");
                const auto reg = RegIndex(static_cast<ScalarReg>(source->Register().index));
                if (reg < _plan.userDataBase) return node;
                node.op = WalkOp::UserData;
                node.immediate = reg - _plan.userDataBase;
                return node;
            }
            case IrOpcode::GetShaderBase: node.op = WalkOp::ShaderBase; return node;
            case IrOpcode::Phi: {
                IrValue* invariant = ResolveInvariantPhi(_plan, &value);
                if (invariant == nullptr) return node;
                node.op = WalkOp::Alias;
                node.args[0] = Node(invariant);
                return node;
            }
            case IrOpcode::ReadFirstLane: {
                node.op = WalkOp::ReadFirstLane;
                IrValue* mask = value.Argument(1);
                node.args[0] = Node(value.Argument(0));
                node.args[1] = mask != nullptr ? Reference(mask) : NoWalkNode;
                return node;
            }
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32:
                node.op = WalkOp::Alias;
                node.args[0] = Node(value.Argument(0));
                return node;
            case IrOpcode::CompositeExtractU64:
            case IrOpcode::CompositeExtractU32x2: return BuildExtract(value, node);
            case IrOpcode::ReadConst: {
                IrValue* slot = Resolved(value, 1);
                if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= _plan.srtReads.size()) return node;
                node.op = WalkOp::ReadConst;
                node.immediate = slot->ImmediateU32();
                node.args[0] = Node(_plan.srtReads[slot->ImmediateU32()].value);
                return node;
            }
            case IrOpcode::LoadAddressU32:
            case IrOpcode::ReadConstBuffer: return IsRawRead(_plan, value) ? BuildRawRead(value, node) : node;
            default: break;
        }
        const auto arity = ComputeArity(value.Opcode());
        if (arity == 0u) return node;
        node.op = WalkOp::Compute;
        node.arity = arity;
        node.maskable = IsRuntimeSelect(value.Opcode()) && value.ArgumentCount() == 3u;
        for (std::uint8_t index = 0; index < arity; ++index) node.args[index] = Node(value.Argument(index));
        return node;
    }

    WalkNode BuildExtract(IrValue& value, WalkNode node) {
        IrValue* index = Resolved(value, 1);
        if (!index->HasImmediate() || index->Type() != IrType::U32) return node;
        const auto component = index->ImmediateU32();
        if (component >= 2u) return node;
        node.immediate = component;
        if (value.Opcode() == IrOpcode::CompositeExtractU64) {
            node.op = WalkOp::ExtractU64;
            node.args[0] = Node(value.Argument(0));
            return node;
        }
        IrValue* source = Resolved(value, 0);
        if (source->Opcode() == IrOpcode::CompositeConstructU32x2) {
            node.op = WalkOp::Alias;
            node.args[0] = Node(source->Argument(component));
        } else if (source->Opcode() == IrOpcode::IAddCarry32) {
            node.op = WalkOp::AddCarry;
            node.args[0] = Node(source->Argument(0));
            node.args[1] = Node(source->Argument(1));
        }
        return node;
    }

    WalkNode BuildRawRead(IrValue& value, WalkNode node) {
        const auto& memory = _plan.memoryInfo.at(value.Flags<MemoryFlags>().index);
        IrValue* handle = Resolved(value, 0);
        if (handle->Opcode() == IrOpcode::Void) return node;
        node.op = WalkOp::RawRead;
        node.immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.offset)));
        node.loadedFromMemory = LoadedFromMemory(*handle);
        node.args[0] = Node(handle->Argument(0));
        node.args[1] = Node(handle->Argument(1));
        node.args[2] = Node(value.Argument(1));
        if (value.Opcode() == IrOpcode::ReadConstBuffer) {
            node.bufferRead = true;
            node.wideHandle = handle->ArgumentCount() == 4u;
            if (node.wideHandle) {
                node.args[3] = Node(handle->Argument(2));
                node.args[4] = Node(handle->Argument(3));
            }
        }
        return node;
    }

    const IrResourcePlan& _plan;
    CompiledWalk& _walk;
    std::unordered_map<const IrValue*, std::uint32_t> _index;
    std::vector<std::uint8_t> _built;
};

// The opcodes the compiler shapes itself; every other one must be computed by Compute exactly
// when Evaluator::EvaluateInst evaluates it.
bool shapedOpcode(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::Identity:
        case IrOpcode::Void:
        case IrOpcode::Phi:
        case IrOpcode::GetUserData:
        case IrOpcode::GetShaderBase:
        case IrOpcode::ReadFirstLane:
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeExtractU32x2:
        case IrOpcode::ReadConst:
        case IrOpcode::LoadAddressU32:
        case IrOpcode::ReadConstBuffer: return true;
        default: return false;
    }
}

// Compute is a copy of the evaluator's arithmetic, and the evaluator changes with upstream: every
// opcode is run through both on a few argument sets, and a difference (an opcode one of them
// does not know, another result) keeps the evaluator in charge.
bool computeMatchesEvaluator() {
    static constexpr std::array<std::array<std::uint32_t, 4>, 7> samples {{
        {5u, 3u, 2u, 1u},
        {0x80000001u, 0xfffffff0u, 31u, 1u},
        {0x3fc00000u, 0x40000000u, 0u, 32u},
        {0x7fc00000u, 0xbf800000u, 33u, 0u},
        {0u, 0u, 0u, 0u},
        {0xffffffffu, 1u, 4u, 28u},
        {0x4f800000u, 0x12345678u, 8u, 8u},
    }};
    const IrResourcePlan plan {};
    const SrtRuntime runtime {};
    for (std::uint32_t code = 0; code < static_cast<std::uint32_t>(IrOpcode::Count); ++code) {
        const auto opcode = static_cast<IrOpcode>(code);
        if (shapedOpcode(opcode)) continue;
        const auto arity = ComputeArity(opcode);
        for (const auto& sample : samples) {
            std::array<IrValue, 4> arguments {IrValue(IrOpcode::Void, IrType::U32, 1u), IrValue(IrOpcode::Void, IrType::U32, 2u), IrValue(IrOpcode::Void, IrType::U32, 3u), IrValue(IrOpcode::Void, IrType::U32, 4u)};
            IrValue inst(opcode, IrType::U32, 5u);
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                arguments[index].SetImmediateU32(sample[index]);
                inst.AddArgument(&arguments[index]);
            }
            std::uint64_t expected = 0;
            bool evaluated = false;
            try {
                Evaluator evaluator(plan, runtime);
                evaluated = evaluator.EvaluateWide(&inst, expected);
            } catch (const std::exception&) {
                std::fprintf(stderr, "[srt] tape off: the evaluator threw on opcode %s\n", std::string(IrOpcodeName(opcode)).c_str());
                return false;
            }
            std::uint64_t computed = 0;
            const bool computes = arity != 0u && Compute(opcode, sample[0], sample[1], sample[2], sample[3], computed);
            if (computes != evaluated || (computes && computed != expected)) {
                std::fprintf(stderr, "[srt] tape off: opcode %s differs from the evaluator (%d 0x%llx against %d 0x%llx)\n", std::string(IrOpcodeName(opcode)).c_str(), computes ? 1 : 0, static_cast<unsigned long long>(computed), evaluated ? 1 : 0, static_cast<unsigned long long>(expected));
                return false;
            }
        }
    }
    return true;
}

bool tapeEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SRT_TAPE") == nullptr && std::getenv("APS5_SRT_DEBUG") == nullptr && computeMatchesEvaluator();
    return enabled;
}

}

std::shared_ptr<const CompiledWalk> CompileWalk(const IrResourcePlan& plan) noexcept {
    try {
        if (!tapeEnabled()) return nullptr;
        auto walk = std::make_shared<CompiledWalk>();
        WalkCompiler(plan, *walk).Run();
        compiledWalks.fetch_add(1, std::memory_order_relaxed);
        return walk;
    } catch (...) {
        refusedWalks.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

CompiledWalkCounts CompiledWalkStatistics() {
    return {compiledWalks.load(std::memory_order_relaxed), refusedWalks.load(std::memory_order_relaxed)};
}

std::uint8_t ComputeArity(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::ConvertF32U32:
        case IrOpcode::ConvertU32F32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPIsNan32:
        case IrOpcode::BitwiseNot32:
        case IrOpcode::LogicalNot: return 1u;
        case IrOpcode::CompositeConstructU64:
        case IrOpcode::IAdd32:
        case IrOpcode::IAdd64:
        case IrOpcode::ISub32:
        case IrOpcode::ISub64:
        case IrOpcode::IMul32:
        case IrOpcode::IMul64:
        case IrOpcode::SMin32:
        case IrOpcode::UMin32:
        case IrOpcode::FPMul32:
        case IrOpcode::FPOrdLessThanEqual32:
        case IrOpcode::FPOrdGreaterThanEqual32:
        case IrOpcode::BitwiseAnd32:
        case IrOpcode::BitwiseAnd64:
        case IrOpcode::BitwiseOr32:
        case IrOpcode::BitwiseXor32:
        case IrOpcode::ShiftLeftLogical32:
        case IrOpcode::ShiftLeftLogical64:
        case IrOpcode::ShiftRightLogical32:
        case IrOpcode::ShiftRightLogical64:
        case IrOpcode::ShiftRightArithmetic32:
        case IrOpcode::ShiftRightArithmetic64:
        case IrOpcode::IEqual32:
        case IrOpcode::INotEqual32:
        case IrOpcode::SLessThan32:
        case IrOpcode::ULessThan32:
        case IrOpcode::UGreaterThan32:
        case IrOpcode::LogicalAnd:
        case IrOpcode::LogicalOr:
        case IrOpcode::LogicalXor: return 2u;
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract:
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectF32: return 3u;
        case IrOpcode::BitFieldInsert: return 4u;
        default: return 0u;
    }
}

bool Compute(IrOpcode opcode, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t& result) {
    switch (opcode) {
        case IrOpcode::CompositeConstructU64: result = static_cast<std::uint32_t>(a) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)) << 32u); return true;
        case IrOpcode::IAdd32: result = static_cast<std::uint32_t>(a + b); return true;
        case IrOpcode::IAdd64: result = a + b; return true;
        case IrOpcode::ISub32: result = static_cast<std::uint32_t>(a - b); return true;
        case IrOpcode::ISub64: result = a - b; return true;
        case IrOpcode::IMul32: result = static_cast<std::uint32_t>(a * b); return true;
        case IrOpcode::IMul64: result = a * b; return true;
        case IrOpcode::SMin32: result = static_cast<std::uint32_t>(std::min(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)), std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(b)))); return true;
        case IrOpcode::UMin32: result = std::min(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)); return true;
        case IrOpcode::ConvertF32U32: result = float32Bits(static_cast<float>(static_cast<std::uint32_t>(a))); return true;
        case IrOpcode::ConvertU32F32: {
            const auto value = float32(a);
            if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > 4294967295.0) {
                return false;
            }
            result = static_cast<std::uint32_t>(value);
            return true;
        }
        case IrOpcode::FPMul32: result = float32Bits(float32(a) * float32(b)); return true;
        case IrOpcode::FPTrunc32: result = float32Bits(std::trunc(float32(a))); return true;
        case IrOpcode::FPIsNan32: result = std::isnan(float32(a)) ? 1u : 0u; return true;
        case IrOpcode::FPOrdLessThanEqual32: result = float32(a) <= float32(b) ? 1u : 0u; return true;
        case IrOpcode::FPOrdGreaterThanEqual32: result = float32(a) >= float32(b) ? 1u : 0u; return true;
        case IrOpcode::BitwiseAnd32: result = static_cast<std::uint32_t>(a & b); return true;
        case IrOpcode::BitwiseAnd64: result = a & b; return true;
        case IrOpcode::BitwiseOr32: result = static_cast<std::uint32_t>(a | b); return true;
        case IrOpcode::BitwiseXor32: result = static_cast<std::uint32_t>(a ^ b); return true;
        case IrOpcode::BitwiseNot32: result = ~static_cast<std::uint32_t>(a); return true;
        case IrOpcode::ShiftLeftLogical32: result = static_cast<std::uint32_t>(a) << (b & 31u); return true;
        case IrOpcode::ShiftLeftLogical64: result = a << (b & 63u); return true;
        case IrOpcode::ShiftRightLogical32: result = static_cast<std::uint32_t>(a) >> (b & 31u); return true;
        case IrOpcode::ShiftRightLogical64: result = a >> (b & 63u); return true;
        case IrOpcode::ShiftRightArithmetic32: result = static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) >> (b & 31u)); return true;
        case IrOpcode::ShiftRightArithmetic64: result = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(a) >> (b & 63u)); return true;
        case IrOpcode::BitFieldUExtract: {
            const auto offset = static_cast<std::uint32_t>(b);
            const auto width = static_cast<std::uint32_t>(c);
            if (offset > 32u || width > 32u - offset) {
                return false;
            }
            const auto mask = width == 32u ? 0xffffffffu : width == 0u ? 0u : (std::uint32_t {1} << width) - 1u;
            result = width == 0u ? 0u : (static_cast<std::uint32_t>(a) >> offset) & mask;
            return true;
        }
        case IrOpcode::BitFieldSExtract: {
            const auto offset = static_cast<std::uint32_t>(b);
            const auto width = static_cast<std::uint32_t>(c);
            if (offset > 32u || width > 32u - offset) {
                return false;
            }
            if (width == 0u) {
                result = 0;
                return true;
            }
            const auto mask = width == 32u ? 0xffffffffu : (std::uint32_t {1} << width) - 1u;
            auto bits = (static_cast<std::uint32_t>(a) >> offset) & mask;
            if (width < 32u && (bits & (std::uint32_t {1} << (width - 1u))) != 0u) {
                bits |= ~mask;
            }
            result = bits;
            return true;
        }
        case IrOpcode::BitFieldInsert: {
            const auto offset = static_cast<std::uint32_t>(c);
            const auto width = static_cast<std::uint32_t>(d);
            if (offset > 32u || width > 32u - offset) {
                return false;
            }
            if (width == 0u) {
                result = static_cast<std::uint32_t>(a);
                return true;
            }
            const auto mask = width == 32u ? 0xffffffffu : ((std::uint32_t {1} << width) - 1u) << offset;
            result = (static_cast<std::uint32_t>(a) & ~mask) | ((static_cast<std::uint32_t>(b) << offset) & mask);
            return true;
        }
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectF32: result = a != 0u ? b : c; return true;
        case IrOpcode::IEqual32: result = static_cast<std::uint32_t>(a) == static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case IrOpcode::INotEqual32: result = static_cast<std::uint32_t>(a) != static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case IrOpcode::SLessThan32: result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) < std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(b)) ? 1u : 0u; return true;
        case IrOpcode::ULessThan32: result = static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case IrOpcode::UGreaterThan32: result = static_cast<std::uint32_t>(a) > static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case IrOpcode::LogicalAnd: result = (a != 0u) && (b != 0u) ? 1u : 0u; return true;
        case IrOpcode::LogicalOr: result = (a != 0u) || (b != 0u) ? 1u : 0u; return true;
        case IrOpcode::LogicalXor: result = (a != 0u) != (b != 0u) ? 1u : 0u; return true;
        case IrOpcode::LogicalNot: result = a == 0u ? 1u : 0u; return true;
        default: return false;
    }
}

CompiledEvaluator::CompiledEvaluator(const CompiledWalk& walk, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots, CompiledEvaluator* cleanEvaluator, std::uint32_t activeMask) : _walk(walk), _nodes(walk.nodes.data()), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask), _values(acquireWalkValues(walk.nodes.size())), _generation(_values->generation) {}

CompiledEvaluator::~CompiledEvaluator() { releaseWalkValues(_values); }

bool CompiledEvaluator::Evaluate(std::uint32_t node, std::uint32_t& result) {
    std::uint64_t wide = 0;
    if (!EvaluateWide(node, wide)) {
        return false;
    }
    result = static_cast<std::uint32_t>(wide);
    return true;
}

bool CompiledEvaluator::EvaluateWide(std::uint32_t index, std::uint64_t& result) {
    const WalkNode& node = _nodes[index];
    if (node.op == WalkOp::Immediate) {
        result = node.immediate;
        return true;
    }
    if (node.op == WalkOp::Fail) {
        return false;
    }
    if (node.maskable && node.args[0] == _activeMask) {
        return EvaluateWide(node.args[1], result);
    }
    auto& slot = _values->slots[index];
    if (slot.stamp == _generation) {
        result = slot.value;
        return true;
    }
    if (slot.visiting == _generation) {
        return false;
    }
    slot.visiting = _generation;
    std::uint64_t out = 0;
    const bool evaluated = Run(node, out);
    slot.visiting = 0;
    if (!evaluated) {
        return false;
    }
    slot.value = out;
    slot.stamp = _generation;
    result = out;
    return true;
}

bool CompiledEvaluator::Run(const WalkNode& node, std::uint64_t& result) {
    switch (node.op) {
        case WalkOp::UserData:
            if (node.immediate >= _runtime.userData.size()) {
                return false;
            }
            result = _runtime.userData[node.immediate];
            return true;
        case WalkOp::ShaderBase: result = _runtime.shaderBase; return true;
        case WalkOp::Alias: return EvaluateWide(node.args[0], result);
        case WalkOp::ReadConst:
            if (node.immediate < _cleanFlatSlots.size() && _cleanFlatSlots[node.immediate] != 0u && _cleanEvaluator != nullptr) {
                return _cleanEvaluator->EvaluateWide(node.args[0], result);
            }
            return EvaluateWide(node.args[0], result);
        case WalkOp::ReadFirstLane: {
            CompiledEvaluator active(_walk, _runtime, _cleanFlatSlots, _cleanEvaluator, node.args[1]);
            active.ReportInaccessibleReads(_inaccessible);
            active.ReportNullRootReads(_nullRoots);
            return active.EvaluateWide(node.args[0], result);
        }
        case WalkOp::ExtractU64: {
            std::uint64_t packed = 0;
            if (!EvaluateWide(node.args[0], packed)) {
                return false;
            }
            result = static_cast<std::uint32_t>(packed >> (node.immediate * 32u));
            return true;
        }
        case WalkOp::AddCarry: {
            std::uint64_t lhs = 0;
            std::uint64_t rhs = 0;
            if (!EvaluateWide(node.args[0], lhs) || !EvaluateWide(node.args[1], rhs)) {
                return false;
            }
            const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lhs)) + static_cast<std::uint32_t>(rhs);
            result = node.immediate == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
            return true;
        }
        case WalkOp::RawRead: return RunRawRead(node, result);
        case WalkOp::Compute: {
            std::array<std::uint64_t, 4> values {};
            for (std::uint8_t index = 0; index < node.arity; ++index) {
                if (!EvaluateWide(node.args[index], values[index])) {
                    return false;
                }
            }
            return Compute(node.opcode, values[0], values[1], values[2], values[3], result);
        }
        default: return false;
    }
}

// Evaluator::EvaluateRawRead, with the handle's shape and LoadedFromMemory taken from the node.
bool CompiledEvaluator::RunRawRead(const WalkNode& node, std::uint64_t& result) {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t offset = 0;
    if (!EvaluateWide(node.args[0], low) || !EvaluateWide(node.args[1], high) || !EvaluateWide(node.args[2], offset)) {
        return false;
    }
    const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
    const auto immediate = static_cast<std::int64_t>(node.immediate);
    std::uint64_t address = 0;
    if (node.bufferRead) {
        std::uint64_t records = 0;
        std::uint64_t word3 = 0;
        if (!node.wideHandle || !EvaluateWide(node.args[3], records) || !EvaluateWide(node.args[4], word3)) {
            return false;
        }
        if (immediate < 0) {
            return false;
        }
        const auto byteOffset = static_cast<std::uint64_t>(immediate) + static_cast<std::uint32_t>(offset);
        const auto aligned = byteOffset & ~std::uint64_t {3};
        const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
        const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
        if (aligned > size || size - aligned < sizeof(std::uint32_t)) {
            result = 0;
            return true;
        }
        address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
    } else {
        const auto relative = (immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
        if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) {
            return false;
        }
    }
    if (_nullRoots != nullptr && base == 0 && !node.loadedFromMemory) {
        ++*_nullRoots;
        result = 0;
        return true;
    }
    if (_inaccessible != nullptr && _runtime.accessible != nullptr && !_runtime.accessible(_runtime.userContext, address, sizeof(std::uint32_t)) && node.loadedFromMemory) {
        *_inaccessible = {node.inst, address};
        return false;
    }
    if (auto* trace = _runtime.readTrace; trace != nullptr) {
        if (node.inst == trace->leaf) trace->leaves.emplace_back(trace->leafSlot, address);
        else trace->otherReads.push_back(address);
    }
    std::uint32_t word = 0;
    if (_runtime.readMemory != nullptr) {
        if (!_runtime.readMemory(_runtime.userContext, address, &word)) {
            return false;
        }
    } else {
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
    }
    result = word;
    return true;
}

}
