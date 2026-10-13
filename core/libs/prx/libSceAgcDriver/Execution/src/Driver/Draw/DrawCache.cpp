#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <condition_variable>
#include <bit>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>

namespace AgcDriver::DriverDetail {

namespace {

// Evicted draw entries and dropped variants are destroyed on a thread of their own, a batch at a
// time: their results free large blocks through the system heap, which cost the preparing thread
// ~8% of its time in busy scenes (under the cache lock). APS5_NO_DRAW_REAPER=1 destroys them on the
// inserting thread right after the lock, as before but outside it.
struct DrawReaper {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<const void>> pending;
};

constexpr std::size_t ReapBatch = 64;

bool drawReaper() {
    static const bool reaper = std::getenv("APS5_NO_DRAW_REAPER") == nullptr;
    return reaper;
}

// Never destroyed: the thread runs until the process ends.
DrawReaper& reaper() {
    static auto* instance = [] {
        auto* created = new DrawReaper();
        std::thread([created] {
            std::vector<std::shared_ptr<const void>> batch;
            for (;;) {
                {
                    std::unique_lock lock(created->mutex);
                    created->wake.wait(lock, [&] { return !created->pending.empty(); });
                    batch.swap(created->pending);
                }
                batch.clear();
            }
        }).detach();
        return created;
    }();
    return *instance;
}

struct ReapTag {};

std::vector<std::shared_ptr<const void>>& reapBatch() {
    return HostThreadLocal<std::vector<std::shared_ptr<const void>>, ReapTag>();
}

// Declared before the cache lock, so it runs after the lock is released.
struct ReapAfter {
    ~ReapAfter() {
        auto& batch = reapBatch();
        if (!drawReaper()) {
            batch.clear();
            return;
        }
        if (batch.size() < ReapBatch) return;
        auto& target = reaper();
        {
            std::lock_guard lock(target.mutex);
            if (target.pending.empty()) target.pending.swap(batch);
            else {
                std::move(batch.begin(), batch.end(), std::back_inserter(target.pending));
                batch.clear();
            }
        }
        target.wake.notify_one();
    }
};

}

bool DrawRecipeRecord::Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const {
    if (stages.size() != variants.size()) return false;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].owner_before(variants[i]) || variants[i].owner_before(stages[i])) return false;
    }
    return true;
}

bool DrawRecipeRecord::Expired() const {
    return std::any_of(stages.begin(), stages.end(), [](const std::weak_ptr<const DispatchVariant>& stage) { return stage.expired(); });
}

bool Driver::drawEntries() {
    static const bool entries = std::getenv("APS5_NO_DRAW_SRT_ENTRIES") == nullptr && !stampValidate();
    return entries;
}

bool Driver::verifyDrawEntries() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_ENTRIES") != nullptr;
    return verify;
}

bool Driver::registerKeyEnabled() {
    static const bool registerKey = std::getenv("APS5_NO_DRAW_KEY") == nullptr;
    return registerKey;
}

bool Driver::verifyDrawRecipe() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr;
    return verify;
}

std::size_t Driver::drawCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{4096};
    }();
    return entries;
}

void Driver::accountDrawVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++drawCacheVariants;
        drawCacheVariantBytes += variantBytes(variant);
    } else {
        --drawCacheVariants;
        drawCacheVariantBytes -= variantBytes(variant);
    }
}

DrawKeySightings::DrawKeySightings(std::size_t keyCount) {
    Graphics::Require(keyCount >= Ways, "draw key sightings need at least one bucket");
    const auto buckets = std::bit_ceil(keyCount / Ways);
    keys.assign(buckets * Ways, 0);
    filled.assign(buckets, 0);
    next.assign(buckets, 0);
    bucketMask = buckets - 1;
}

bool DrawKeySightings::SeenBefore(std::uint64_t key) {
    auto mixed = key;
    mixed = (mixed ^ (mixed >> 30u)) * 0xbf58476d1ce4e5b9ull;
    mixed = (mixed ^ (mixed >> 27u)) * 0x94d049bb133111ebull;
    mixed ^= mixed >> 31u;
    const auto bucket = static_cast<std::size_t>(mixed) & bucketMask;
    auto* ways = keys.data() + bucket * Ways;
    if (std::find(ways, ways + filled[bucket], key) != ways + filled[bucket]) return true;
    ways[next[bucket]] = key;
    next[bucket] = static_cast<std::uint8_t>((next[bucket] + 1u) % Ways);
    if (filled[bucket] < Ways) ++filled[bucket];
    return false;
}

bool Driver::admitDrawKey(std::uint64_t key) {
    std::lock_guard cacheLock(drawCacheMutex);
    if (drawCache.contains(key)) return true;
    if (!drawKeySightings) drawKeySightings.emplace(drawCacheEntries() * DrawKeySightings::Ways);
    return drawKeySightings->SeenBefore(key);
}

void Driver::insertDrawEntry(std::uint64_t key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode) {
    const ReapAfter reapAfter;
    auto& garbage = reapBatch();
    std::lock_guard cacheLock(drawCacheMutex);
    auto& counters = drawEntryCounters;
    ++counters.inserts;
    const auto found = drawCache.find(key);
    auto replacement = std::make_shared<DrawEntry>();
    replacement->stages.resize(fresh.size());
    replacement->decode = std::move(decode);
    if (found != drawCache.end()) {
        if (found->second->stages.size() == fresh.size()) replacement->stages = found->second->stages;
        if (found->second->decode != nullptr) replacement->decode = found->second->decode;
        replacement->recipes.store(found->second->recipes.load());
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (fresh[i] == nullptr) continue;
        auto& variants = replacement->stages[i];
        const auto present = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == fresh[i]->pushOffset && kept->runs == fresh[i]->runs && kept->words == fresh[i]->words; });
        if (present != variants.end()) {
            ++counters.present;
            fresh[i] = *present;
            continue;
        }
        accountDrawVariant(*fresh[i], true);
        variants.insert(variants.begin(), fresh[i]);
        ++counters.variantsInserted;
        while (variants.size() > dispatchVariants()) {
            accountDrawVariant(*variants.back(), false);
            garbage.push_back(std::move(variants.back()));
            variants.pop_back();
            ++counters.variantsEvicted;
        }
    }
    replacement->touched = drawCacheHits;
    if (found == drawCache.end()) {
        drawOrder.push_front(key);
        replacement->order = drawOrder.begin();
        drawCache.emplace(key, std::move(replacement));
        ++drawKeyFilter[drawKeySlot(key)];
    } else {
        replacement->order = found->second->order;
        drawOrder.splice(drawOrder.begin(), drawOrder, replacement->order);
        garbage.push_back(std::move(found->second));
        found->second = std::move(replacement);
    }
    while (drawCache.size() > drawCacheEntries()) {
        const auto last = drawCache.find(drawOrder.back());
        for (const auto& variants : last->second->stages) {
            for (const auto& variant : variants) accountDrawVariant(*variant, false);
        }
        drawOrder.erase(last->second->order);
        --drawKeyFilter[drawKeySlot(last->first)];
        garbage.push_back(std::move(last->second));
        drawCache.erase(last);
        ++drawCacheEvictions;
    }
}

void Driver::eraseDrawEntry(std::uint64_t key, const std::shared_ptr<DrawEntry>& expected) {
    const auto found = drawCache.find(key);
    if (found == drawCache.end() || found->second != expected) return;
    for (const auto& variants : found->second->stages) {
        for (const auto& variant : variants) accountDrawVariant(*variant, false);
    }
    --drawKeyFilter[drawKeySlot(key)];
    drawOrder.erase(found->second->order);
    drawCache.erase(found);
}

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry&) {
    return std::all_of(decode.programs.begin(), decode.programs.end(), [&](const DrawProgram& program) {
        return !program.snapshot->header.empty() || ReadRawShader(program.snapshot->codeAddress + program.codeOffset * sizeof(std::uint32_t)) == program.snapshot;
    });
}

std::shared_ptr<const DrawRecipe> Driver::findDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages) {
    std::shared_ptr<const std::vector<DrawRecipeRecord>> records;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return nullptr;
        records = found->second->recipes.load();
    }
    if (records == nullptr) return nullptr;
    for (const auto& record : *records) {
        if (record.Matches(stages)) return record.recipe;
    }
    return nullptr;
}

void Driver::attachDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe) {
    std::shared_ptr<DrawEntry> entry;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return;
        entry = found->second;
    }
    const auto old = entry->recipes.load();
    auto records = std::make_shared<std::vector<DrawRecipeRecord>>();
    records->push_back({std::vector<std::weak_ptr<const DispatchVariant>>(stages.begin(), stages.end()), std::move(recipe)});
    if (old != nullptr) {
        for (const auto& record : *old) {
            if (record.Matches(stages) || record.Expired() || records->size() >= dispatchVariants()) continue;
            records->push_back(record);
        }
    }
    entry->recipes.store(std::move(records));
    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, VulkanDevice::RecipeKind::Draw);
}

}
