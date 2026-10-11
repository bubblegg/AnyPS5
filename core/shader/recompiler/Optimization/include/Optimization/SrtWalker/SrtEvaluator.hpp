#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

// A walk's values by IrValue id: arrays a thread's evaluators take turns with, so a walk neither
// hashes nor allocates once the arrays have grown (the hash table below cost the draw path's
// preparing thread 4% in inserts alone, its growth included). A slot belongs to the current walk
// when its stamp is the arrays' generation.
struct DenseValues {
    static constexpr std::uint32_t MaxId = 16384;
    std::vector<const IrValue*> keys;
    std::vector<std::uint64_t> values;
    std::vector<std::uint32_t> stamps;
    std::uint32_t generation = 0;
};
// The calling thread's spare arrays (or new ones) with a fresh generation, and their return.
DenseValues* AcquireDenseValues();
void ReleaseDenseValues(DenseValues* values) noexcept;

class EvaluatedValues {
public:
    EvaluatedValues() : _dense(AcquireDenseValues()) {}
    ~EvaluatedValues() { ReleaseDenseValues(_dense); }
    EvaluatedValues(const EvaluatedValues&) = delete;
    EvaluatedValues& operator=(const EvaluatedValues&) = delete;

    bool Find(const IrValue* key, std::uint64_t& value) const {
        const auto id = key->Id();
        if (id < _dense->stamps.size() && _dense->stamps[id] == _dense->generation) {
            if (_dense->keys[id] == key) {
                value = _dense->values[id];
                return true;
            }
            return FindSpilled(key, value);
        }
        return id >= DenseValues::MaxId && FindSpilled(key, value);
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        const auto id = key->Id();
        if (id < DenseValues::MaxId) {
            if (id >= _dense->stamps.size()) {
                const auto size = std::min<std::size_t>(DenseValues::MaxId, std::max<std::size_t>({std::size_t{256}, _dense->stamps.size() * 2u, std::size_t{id} + 1u}));
                _dense->keys.resize(size);
                _dense->values.resize(size);
                _dense->stamps.resize(size);
            }
            if (_dense->stamps[id] != _dense->generation) {
                _dense->stamps[id] = _dense->generation;
                _dense->keys[id] = key;
                _dense->values[id] = value;
                return;
            }
            if (_dense->keys[id] == key) {
                return;
            }
        }
        InsertSpilled(key, value);
    }

private:
    // Values whose id is beyond the arrays or already taken by another value of the walk.
    bool FindSpilled(const IrValue* key, std::uint64_t& value) const {
        if (_slots.empty()) {
            return false;
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            const auto& slot = _slots[index];
            if (slot.key == key) {
                value = slot.value;
                return true;
            }
            if (slot.key == nullptr) {
                return false;
            }
        }
    }
    void InsertSpilled(const IrValue* key, std::uint64_t value) {
        if ((_count + 1u) * 2u > _slots.size()) {
            Grow();
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
                return;
            }
            if (slot.key == nullptr) {
                slot = {key, value};
                ++_count;
                return;
            }
        }
    }
    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
    };
    std::size_t Home(const IrValue* key) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (_slots.size() - 1u);
    }
    void Grow() {
        std::vector<Slot> previous(_slots.empty() ? 64u : _slots.size() * 2u);
        previous.swap(_slots);
        _count = 0;
        for (const auto& slot : previous) {
            if (slot.key != nullptr) {
                InsertSpilled(slot.key, slot.value);
            }
        }
    }
    DenseValues* _dense;
    std::vector<Slot> _slots;
    std::size_t _count = 0;
};

struct InaccessibleRead {
    const IrValue* read = nullptr;
    std::uint64_t address = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr) {}

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);
    void ReportInaccessibleReads(InaccessibleRead* sink) { _inaccessible = sink; }
    void ReportNullRootReads(std::uint32_t* sink) { _nullRoots = sink; }

private:
    static float Float32(std::uint64_t bits);
    static std::uint64_t Float32Bits(float value);

    bool Arg(IrValue& inst, std::size_t index, std::uint64_t& result);
    bool EvaluatePhi(IrValue& inst, std::uint64_t& result);
    bool EvaluateExtract(IrValue& inst, std::uint64_t& result);
    bool EvaluateRawRead(IrValue& inst, std::uint64_t& result);
    bool EvaluateInst(IrValue& inst, std::uint64_t& result);

    const IrResourcePlan& _program;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    Evaluator* _cleanEvaluator = nullptr;
    IrValue* _activeMask = nullptr;
    InaccessibleRead* _inaccessible = nullptr;
    std::uint32_t* _nullRoots = nullptr;
    EvaluatedValues _cache;
    std::vector<IrValue*> _visiting;
};

}

#endif
