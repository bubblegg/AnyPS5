#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

// A walk's values, by IrValue: an open-addressing table a thread's evaluators take turns with. It
// keeps its size from walk to walk, so a walk neither allocates nor rehashes once the table has
// grown (starting empty and growing cost the draw path's preparing thread 4% in inserts alone);
// a slot belongs to the current walk when its stamp is the table's generation. Indexing by
// IrValue id instead was slower: the ids are sparse, and the arrays did not stay in the cache.
struct ValueTable {
    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
        std::uint32_t stamp = 0;
    };
    // The first `capacity` slots are in use: a walk starts with room for twice what the table's
    // last walk held, so a small walk stays within a few cache lines of a table a large one grew.
    std::vector<Slot> slots;
    std::vector<Slot> moving;
    std::size_t capacity = 0;
    std::size_t count = 0;
    std::size_t lastCount = 0;
    std::uint32_t generation = 0;
    void NextGeneration() {
        if (++generation == 0) {
            for (auto& slot : slots) slot.stamp = 0;
            generation = 1;
        }
    }
};
// The calling thread's spare table (or a new one) with a fresh generation, and its return.
ValueTable* AcquireValueTable();
void ReleaseValueTable(ValueTable* table) noexcept;

class EvaluatedValues {
public:
    EvaluatedValues() : _table(AcquireValueTable()) {}
    ~EvaluatedValues() { ReleaseValueTable(_table); }
    EvaluatedValues(const EvaluatedValues&) = delete;
    EvaluatedValues& operator=(const EvaluatedValues&) = delete;

    bool Find(const IrValue* key, std::uint64_t& value) const {
        const auto& slots = _table->slots;
        for (std::size_t index = Home(key);; index = (index + 1u) & (_table->capacity - 1u)) {
            const auto& slot = slots[index];
            if (slot.stamp != _table->generation) {
                return false;
            }
            if (slot.key == key) {
                value = slot.value;
                return true;
            }
        }
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        if ((_table->count + 1u) * 2u > _table->capacity) {
            Grow();
        }
        auto& slots = _table->slots;
        for (std::size_t index = Home(key);; index = (index + 1u) & (_table->capacity - 1u)) {
            auto& slot = slots[index];
            if (slot.stamp != _table->generation) {
                slot = {key, value, _table->generation};
                ++_table->count;
                return;
            }
            if (slot.key == key) {
                return;
            }
        }
    }

private:
    std::size_t Home(const IrValue* key) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (_table->capacity - 1u);
    }
    void Grow() {
        auto& moving = _table->moving;
        moving.clear();
        for (std::size_t index = 0; index < _table->capacity; ++index) {
            if (_table->slots[index].stamp == _table->generation) {
                moving.push_back(_table->slots[index]);
            }
        }
        _table->capacity *= 2u;
        if (_table->slots.size() < _table->capacity) {
            _table->slots.resize(_table->capacity);
        }
        _table->NextGeneration();
        _table->count = 0;
        for (const auto& slot : moving) {
            Insert(slot.key, slot.value);
        }
    }
    ValueTable* _table;
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
