// Debug aid: APS5_GUARD_NEW=1 appends a canary, the allocating address and a signature to every
// block this library allocates with operator new, checks them when the block is deleted here, and
// keeps the deleted block in a quarantine (APS5_GUARD_QUARANTINE_MB, default 768) before it goes
// back to the heap. It stops the process with the addresses involved (offsets into this library)
// when
//  - a block's canary was trampled (an overrun: the allocating code owns the buffer);
//  - a sized delete names another size than the live block's (the deleted object was freed before
//    and its memory belongs to another owner now);
//  - a block is deleted while it is in the quarantine (a double delete: both deleting addresses);
//  - a quarantined block's fill was written before it left the quarantine (a use after free).
// The block's address is malloc's own, so the C++ runtime library may free it as usual; a block
// without the signature (allocated elsewhere, or overrun past the whole tail) is freed unchecked.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <utility>
#include <windows.h>
#include <x86intrin.h>

extern "C" char __ImageBase;

namespace {

constexpr std::uint64_t Signature = 0x4150533547554152ull;
constexpr std::uint64_t FreedSignature = 0x4150533546524545ull;
constexpr std::size_t CanaryBytes = 16;
constexpr std::size_t TailBytes = CanaryBytes + 16;
constexpr unsigned char CanaryByte = 0xa5;
constexpr unsigned char FreedByte = 0xdd;
constexpr std::size_t FillBytes = 64;

bool guardEnabled() {
    static const bool enabled = std::getenv("APS5_GUARD_NEW") != nullptr;
    return enabled;
}

unsigned long long offsetOf(const void* address) {
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(&__ImageBase));
}

std::uint64_t signatureOf(std::uint64_t kind, const unsigned char* block, std::size_t size) {
    return kind ^ reinterpret_cast<std::uintptr_t>(block) ^ (static_cast<std::uint64_t>(size) << 20u);
}

[[noreturn]] void stop() {
    std::fflush(stderr);
    std::abort();
}

// The quarantine: a ring of deleted blocks, oldest out first once their bytes exceed the budget.
struct Quarantine {
    static constexpr std::size_t Slots = std::size_t{1} << 21u;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    unsigned char** blocks = nullptr;
    std::size_t head = 0;
    std::size_t count = 0;
    std::size_t bytes = 0;
    std::size_t budget = 0;
};

Quarantine& quarantine() {
    static Quarantine* instance = [] {
        auto* created = static_cast<Quarantine*>(std::calloc(1, sizeof(Quarantine)));
        created->blocks = static_cast<unsigned char**>(std::calloc(Quarantine::Slots, sizeof(unsigned char*)));
        const char* text = std::getenv("APS5_GUARD_QUARANTINE_MB");
        const auto megabytes = text != nullptr ? std::strtoull(text, nullptr, 10) : 768ull;
        created->budget = static_cast<std::size_t>(megabytes) << 20u;
        return created;
    }();
    return *instance;
}

// A quarantined block's tail: [8 bytes first deleting address][8 bytes canary][8 allocating address][8 freed signature].
void releaseQuarantined(unsigned char* block) {
    const std::size_t total = _msize(block);
    const std::size_t size = total - TailBytes;
    const std::size_t filled = size < FillBytes ? size : FillBytes;
    for (std::size_t i = 0; i < filled; ++i) {
        if (block[i] == FreedByte) continue;
        void* freedAt = nullptr;
        void* allocatedAt = nullptr;
        std::memcpy(&freedAt, block + size, sizeof(freedAt));
        std::memcpy(&allocatedAt, block + size + CanaryBytes, sizeof(allocatedAt));
        std::fprintf(stderr, "[heap-guard] use after free: a block of %llu bytes allocated at libSceAgcDriver.prx+0x%llx and deleted at +0x%llx was written afterwards (byte %zu); its first bytes now:", static_cast<unsigned long long>(size), offsetOf(allocatedAt), offsetOf(freedAt), i);
        for (std::size_t b = 0; b < filled && b < 32; ++b) std::fprintf(stderr, " %02x", block[b]);
        std::fprintf(stderr, "\n");
        stop();
    }
    std::memset(block + size + CanaryBytes + 8, 0, 8);
    std::free(block);
}

void quarantineBlock(unsigned char* block, std::size_t size) {
    auto& q = quarantine();
    unsigned char* evicted[16];
    std::size_t evictedCount = 0;
    while (q.lock.test_and_set(std::memory_order_acquire)) {
    }
    while ((q.count == Quarantine::Slots || q.bytes + size > q.budget) && q.count != 0 && evictedCount < 16) {
        auto* oldest = q.blocks[q.head];
        q.head = (q.head + 1) % Quarantine::Slots;
        --q.count;
        q.bytes -= _msize(oldest);
        evicted[evictedCount++] = oldest;
    }
    const bool kept = q.count < Quarantine::Slots && q.bytes + size + TailBytes <= q.budget;
    if (kept) {
        q.blocks[(q.head + q.count) % Quarantine::Slots] = block;
        ++q.count;
        q.bytes += size + TailBytes;
    }
    q.lock.clear(std::memory_order_release);
    for (std::size_t i = 0; i < evictedCount; ++i) releaseQuarantined(evicted[i]);
    if (!kept) releaseQuarantined(block);
}

// Profiling aid: APS5_PROFILE_ALLOC=1 counts this library's allocations by size (powers of two) and
// the cycles malloc took, and prints them every 2^21 allocations.
bool allocProfiled() {
    static const bool enabled = std::getenv("APS5_PROFILE_ALLOC") != nullptr;
    return enabled;
}

struct AllocProfile {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> cycles{0};
    std::atomic<std::uint64_t> bytes{0};
    std::array<std::atomic<std::uint64_t>, 24> bySize{};
    std::array<std::atomic<std::uint64_t>, 24> cyclesBySize{};
    std::atomic<long long> startedAt{0};
};

AllocProfile& allocProfile() {
    static auto* profile = static_cast<AllocProfile*>(std::calloc(1, sizeof(AllocProfile)));
    return *profile;
}

void noteAllocation(std::size_t size, std::uint64_t cycles) {
    auto& p = allocProfile();
    std::size_t bucket = 0;
    while (bucket + 1 < p.bySize.size() && (std::size_t{1} << (bucket + 4)) < size) ++bucket;
    p.bySize[bucket].fetch_add(1, std::memory_order_relaxed);
    p.cyclesBySize[bucket].fetch_add(cycles, std::memory_order_relaxed);
    p.cycles.fetch_add(cycles, std::memory_order_relaxed);
    p.bytes.fetch_add(size, std::memory_order_relaxed);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    long long expected = 0;
    p.startedAt.compare_exchange_strong(expected, now.QuadPart);
    const auto count = p.count.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & ((std::uint64_t{1} << 21u) - 1u)) != 0) return;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double seconds = static_cast<double>(now.QuadPart - p.startedAt.exchange(now.QuadPart)) / static_cast<double>(frequency.QuadPart);
    const auto total = p.cycles.exchange(0);
    std::fprintf(stderr, "[alloc] %llu allocations by this library in %.1f s (%.0f per s), %.1f MiB, malloc %.0f cycles each on average; by size (<= bytes: count, mean cycles):", static_cast<unsigned long long>(std::uint64_t{1} << 21u), seconds, static_cast<double>(std::uint64_t{1} << 21u) / seconds, static_cast<double>(p.bytes.exchange(0)) / 1048576.0, static_cast<double>(total) / static_cast<double>(std::uint64_t{1} << 21u));
    for (std::size_t i = 0; i < p.bySize.size(); ++i) {
        const auto n = p.bySize[i].exchange(0);
        const auto c = p.cyclesBySize[i].exchange(0);
        if (n != 0) std::fprintf(stderr, " %llu: %llu, %.0f", static_cast<unsigned long long>(std::uint64_t{1} << (i + 4)), static_cast<unsigned long long>(n), static_cast<double>(c) / static_cast<double>(n));
    }
    std::fprintf(stderr, "\n");
}

// A per-thread cache of small freed blocks. The draw path makes hundreds of small allocations per
// draw (millions a second), and malloc plus free cost it about a quarter of its time. Every block in
// the cache is a whole malloc block that holds its class size, so a block handed out from the cache
// may still be freed by anyone with free(), as the C++ runtime library does with some blocks this
// library allocates. Off under APS5_GUARD_NEW and with APS5_NO_ALLOC_CACHE.
//
// With pipelined draws one thread allocates what another frees, so a thread's own cache would stay
// empty on one side and full on the other. Each class therefore keeps two magazines of blocks per
// thread; a thread with two full ones hands one to a depot shared by all threads, and a thread
// with two empty ones takes a full one from it.
//
// A magazine is an array of block addresses, not a list threaded through the blocks: a block that
// comes back from another thread is cold in this thread's cache, and following a link inside it
// stalled the taker on every allocation (8% of the preparing thread). The next block to be handed
// out is prefetched instead.
//
// The class of a freed block is the largest it fully holds. Its size comes from _msize, which reads
// the heap's header of the block (another cold line, 9% of the preparing thread), or from the size
// a sized delete names: the block holds at least that. APS5_VERIFY_SIZED_DELETE=1 compares the two
// and reports a delete that names more than its block holds (none in 335 million deletes of a play
// session on 2026-10-12; its shared counters slow the run down by a quarter). APS5_NO_SIZED_DELETE=1
// asks _msize for every block again.
struct Magazine {
    static constexpr std::uint32_t Blocks = 62;
    Magazine* next;
    std::uint32_t count;
    void* blocks[Blocks];
};

struct SmallCache {
    static constexpr std::size_t Classes = 64;
    static constexpr std::size_t Granule = 8;
    static constexpr std::size_t MaxBytes = Classes * Granule;
    static constexpr std::uint32_t DepotMagazines = 64;
    static constexpr std::uint32_t EmptyMagazines = 1024;
    Magazine* current[Classes];
    Magazine* spare[Classes];
};

struct Depot {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Magazine* full = nullptr;
    std::uint32_t count = 0;
};

Depot depots[SmallCache::Classes];
// Empty magazines, of any class.
Depot emptyMagazines;

void lockDepot(Depot& depot) {
    while (depot.lock.test_and_set(std::memory_order_acquire)) _mm_pause();
}

Magazine* takeEmptyMagazine() {
    Magazine* magazine = nullptr;
    if (emptyMagazines.count != 0) {
        lockDepot(emptyMagazines);
        magazine = emptyMagazines.full;
        if (magazine != nullptr) {
            emptyMagazines.full = magazine->next;
            --emptyMagazines.count;
        }
        emptyMagazines.lock.clear(std::memory_order_release);
    }
    if (magazine == nullptr) magazine = static_cast<Magazine*>(std::malloc(sizeof(Magazine)));
    if (magazine != nullptr) {
        magazine->next = nullptr;
        magazine->count = 0;
    }
    return magazine;
}

void putEmptyMagazine(Magazine* magazine) {
    lockDepot(emptyMagazines);
    const bool kept = emptyMagazines.count < SmallCache::EmptyMagazines;
    if (kept) {
        magazine->next = emptyMagazines.full;
        emptyMagazines.full = magazine;
        ++emptyMagazines.count;
    }
    emptyMagazines.lock.clear(std::memory_order_release);
    if (!kept) std::free(magazine);
}

void freeBlocks(Magazine* magazine) {
    for (std::uint32_t i = 0; i < magazine->count; ++i) std::free(magazine->blocks[i]);
    magazine->count = 0;
}

// Takes a full magazine; its blocks are freed when the depot holds enough of the class.
void depotPut(std::size_t c, Magazine* magazine) {
    auto& depot = depots[c];
    lockDepot(depot);
    const bool kept = depot.count < SmallCache::DepotMagazines;
    if (kept) {
        magazine->next = depot.full;
        depot.full = magazine;
        ++depot.count;
    }
    depot.lock.clear(std::memory_order_release);
    if (!kept) {
        freeBlocks(magazine);
        putEmptyMagazine(magazine);
    }
}

Magazine* depotTake(std::size_t c) {
    auto& depot = depots[c];
    if (depot.count == 0) return nullptr;
    lockDepot(depot);
    Magazine* magazine = depot.full;
    if (magazine != nullptr) {
        depot.full = magazine->next;
        --depot.count;
    }
    depot.lock.clear(std::memory_order_release);
    return magazine;
}

void WINAPI flushSmallCache(void* data) {
    auto* cache = static_cast<SmallCache*>(data);
    if (cache == nullptr) return;
    for (std::size_t c = 0; c < SmallCache::Classes; ++c) {
        for (Magazine* magazine : {cache->current[c], cache->spare[c]}) {
            if (magazine == nullptr) continue;
            freeBlocks(magazine);
            std::free(magazine);
        }
    }
    std::free(cache);
}

bool smallCacheEnabled() {
    static const bool enabled = !guardEnabled() && std::getenv("APS5_NO_ALLOC_CACHE") == nullptr;
    return enabled;
}

DWORD smallCacheSlot() {
    static const DWORD slot = FlsAlloc(&flushSmallCache);
    return slot;
}

SmallCache* smallCache(bool create) {
    const DWORD slot = smallCacheSlot();
    if (slot == FLS_OUT_OF_INDEXES) return nullptr;
    auto* cache = static_cast<SmallCache*>(FlsGetValue(slot));
    if (cache == nullptr && create) {
        cache = static_cast<SmallCache*>(std::calloc(1, sizeof(SmallCache)));
        if (cache != nullptr && !FlsSetValue(slot, cache)) {
            std::free(cache);
            cache = nullptr;
        }
    }
    return cache;
}

void* takeCached(std::size_t size) {
    if (size > SmallCache::MaxBytes) return nullptr;
    auto* cache = smallCache(false);
    if (cache == nullptr) return nullptr;
    const std::size_t c = (size != 0 ? size - 1 : 0) / SmallCache::Granule;
    Magazine* magazine = cache->current[c];
    if (magazine == nullptr || magazine->count == 0) {
        if (Magazine* spare = cache->spare[c]; spare != nullptr && spare->count != 0) {
            cache->spare[c] = magazine;
            cache->current[c] = magazine = spare;
        } else {
            Magazine* full = depotTake(c);
            if (full == nullptr) return nullptr;
            if (magazine != nullptr) putEmptyMagazine(magazine);
            cache->current[c] = magazine = full;
        }
    }
    void* block = magazine->blocks[--magazine->count];
    if (magazine->count != 0) __builtin_prefetch(magazine->blocks[magazine->count - 1], 1);
    return block;
}

bool sizedDelete() {
    static const bool enabled = std::getenv("APS5_NO_SIZED_DELETE") == nullptr && std::getenv("APS5_VERIFY_SIZED_DELETE") == nullptr;
    return enabled;
}

// APS5_VERIFY_SIZED_DELETE=1: every sized delete's size against its block's, and what classing by
// the delete's size would do, every 2^22 deletes.
struct SizedDeleteCheck {
    std::atomic<std::uint64_t> sized{0}, unsized{0}, violations{0}, exact{0}, lower{0}, uncached{0};
};

bool verifySizedDelete() {
    static const bool enabled = std::getenv("APS5_VERIFY_SIZED_DELETE") != nullptr;
    return enabled;
}

void checkSizedDelete(void* block, std::size_t capacity, std::size_t deleting, void* site) {
    static auto* check = new (std::calloc(1, sizeof(SizedDeleteCheck))) SizedDeleteCheck();
    if (deleting == static_cast<std::size_t>(-1)) {
        check->unsized.fetch_add(1, std::memory_order_relaxed);
    } else {
        const auto count = check->sized.fetch_add(1, std::memory_order_relaxed) + 1;
        if (deleting > capacity) {
            if (check->violations.fetch_add(1, std::memory_order_relaxed) < 40) std::fprintf(stderr, "[alloc] sized delete names more than its block holds: %llu bytes deleted at libSceAgcDriver.prx+0x%llx, the block %p holds %llu\n", static_cast<unsigned long long>(deleting), offsetOf(site), block, static_cast<unsigned long long>(capacity));
        } else if (deleting < SmallCache::Granule || deleting > SmallCache::MaxBytes) {
            if (capacity >= SmallCache::Granule && capacity <= SmallCache::MaxBytes) check->uncached.fetch_add(1, std::memory_order_relaxed);
        } else if (deleting / SmallCache::Granule == capacity / SmallCache::Granule) {
            check->exact.fetch_add(1, std::memory_order_relaxed);
        } else {
            check->lower.fetch_add(1, std::memory_order_relaxed);
        }
        if ((count & ((std::uint64_t{1} << 22u) - 1u)) == 0) std::fprintf(stderr, "[alloc] sized deletes so far: %llu sized, %llu unsized; %llu named more than the block holds; of the sized, by the class the delete's size gives: %llu the block's own, %llu a smaller one, %llu none where the block has one\n", static_cast<unsigned long long>(count), static_cast<unsigned long long>(check->unsized.load()), static_cast<unsigned long long>(check->violations.load()), static_cast<unsigned long long>(check->exact.load()), static_cast<unsigned long long>(check->lower.load()), static_cast<unsigned long long>(check->uncached.load()));
    }
}

// Keeps a freed block in the cache of the class it fully holds; false if it has none or the cache
// has no room. `deleting` is the size a sized delete named, or -1.
bool keepCached(void* block, std::size_t deleting, void* site) {
    std::size_t capacity = 0;
    // A size that is no whole number of granules says too little: the block may hold the granule
    // it ends in (one of this library's does), so the heap is asked. Over the largest class it
    // says enough: the block has no class.
    if (deleting != static_cast<std::size_t>(-1) && sizedDelete() && (deleting > SmallCache::MaxBytes || (deleting >= SmallCache::Granule && deleting % SmallCache::Granule == 0))) {
        capacity = deleting;
    } else {
        capacity = _msize(block);
        if (verifySizedDelete()) checkSizedDelete(block, capacity, deleting, site);
    }
    if (capacity < SmallCache::Granule || capacity > SmallCache::MaxBytes) return false;
    const std::size_t c = capacity / SmallCache::Granule - 1;
    auto* cache = smallCache(true);
    if (cache == nullptr) return false;
    Magazine* magazine = cache->current[c];
    if (magazine == nullptr || magazine->count == Magazine::Blocks) {
        Magazine* spare = cache->spare[c];
        if (spare != nullptr && spare->count == 0) {
            cache->spare[c] = magazine;
            cache->current[c] = magazine = spare;
        } else {
            Magazine* empty = takeEmptyMagazine();
            if (empty == nullptr) return false;
            if (spare != nullptr) depotPut(c, spare);
            cache->spare[c] = magazine;
            cache->current[c] = magazine = empty;
        }
    }
    magazine->blocks[magazine->count++] = block;
    return true;
}

void* guardedAllocate(std::size_t size, void* site, bool nothrow) {
    const bool guard = guardEnabled();
    const auto started = allocProfiled() ? __rdtsc() : 0ull;
    if (smallCacheEnabled()) {
        if (void* cached = takeCached(size)) {
            if (allocProfiled()) noteAllocation(size, __rdtsc() - started);
            return cached;
        }
        // A miss allocates the whole class, so the block is reused for any size of its class.
        if (size <= SmallCache::MaxBytes) size = std::max<std::size_t>((size + SmallCache::Granule - 1) / SmallCache::Granule * SmallCache::Granule, SmallCache::Granule);
    }
    auto* block = static_cast<unsigned char*>(std::malloc(guard ? size + TailBytes : size != 0 ? size : 1));
    if (allocProfiled()) noteAllocation(size, __rdtsc() - started);
    if (block == nullptr) {
        if (nothrow) return nullptr;
        throw std::bad_alloc();
    }
    if (guard) {
        const std::uint64_t signature = signatureOf(Signature, block, size);
        std::memset(block + size, CanaryByte, CanaryBytes);
        std::memcpy(block + size + CanaryBytes, &site, sizeof(site));
        std::memcpy(block + size + CanaryBytes + 8, &signature, sizeof(signature));
    }
    return block;
}

void guardedFree(void* pointer, void* site, std::size_t deleting) noexcept {
    if (pointer == nullptr) return;
    if (!guardEnabled()) {
        if (smallCacheEnabled() && keepCached(pointer, deleting, site)) return;
        std::free(pointer);
        return;
    }
    auto* block = static_cast<unsigned char*>(pointer);
    const std::size_t total = _msize(pointer);
    if (total < TailBytes || total == static_cast<std::size_t>(-1)) {
        std::free(pointer);
        return;
    }
    const std::size_t size = total - TailBytes;
    std::uint64_t signature = 0;
    void* allocatedAt = nullptr;
    std::memcpy(&signature, block + size + CanaryBytes + 8, sizeof(signature));
    std::memcpy(&allocatedAt, block + size + CanaryBytes, sizeof(allocatedAt));
    if (signature == signatureOf(FreedSignature, block, size)) {
        void* freedAt = nullptr;
        std::memcpy(&freedAt, block + size, sizeof(freedAt));
        std::fprintf(stderr, "[heap-guard] double delete: a block of %llu bytes allocated at libSceAgcDriver.prx+0x%llx was deleted at +0x%llx and is deleted again at +0x%llx", static_cast<unsigned long long>(size), offsetOf(allocatedAt), offsetOf(freedAt), offsetOf(site));
        if (deleting != static_cast<std::size_t>(-1)) std::fprintf(stderr, " (as %llu bytes)", static_cast<unsigned long long>(deleting));
        std::fprintf(stderr, "\n");
        stop();
    }
    if (signature != signatureOf(Signature, block, size)) {
        std::free(pointer);
        return;
    }
    // A sized delete names the size of the object it destroys: another size means this block
    // belongs to a different owner now, so the caller is deleting something a second time.
    if (deleting != static_cast<std::size_t>(-1) && deleting != size) {
        std::fprintf(stderr, "[heap-guard] a delete of %llu bytes at libSceAgcDriver.prx+0x%llx hit a live block of %llu bytes that was allocated at +0x%llx: the deleted object was already freed once\n", static_cast<unsigned long long>(deleting), offsetOf(site), static_cast<unsigned long long>(size), offsetOf(allocatedAt));
        stop();
    }
    for (std::size_t i = 0; i < CanaryBytes; ++i) {
        if (block[size + i] == CanaryByte) continue;
        std::fprintf(stderr, "[heap-guard] a block of %llu bytes was overrun: allocated at libSceAgcDriver.prx+0x%llx, deleted at +0x%llx; the %zu bytes after it:", static_cast<unsigned long long>(size), offsetOf(allocatedAt), offsetOf(site), CanaryBytes);
        for (std::size_t b = 0; b < CanaryBytes; ++b) std::fprintf(stderr, " %02x", block[size + b]);
        std::fprintf(stderr, "\n");
        stop();
    }
    const std::uint64_t freed = signatureOf(FreedSignature, block, size);
    std::memcpy(block + size, &site, sizeof(site));
    std::memcpy(block + size + CanaryBytes + 8, &freed, sizeof(freed));
    std::memset(block, FreedByte, size < FillBytes ? size : FillBytes);
    quarantineBlock(block, size);
}

}

void* operator new(std::size_t size) { return guardedAllocate(size, __builtin_return_address(0), false); }
void* operator new[](std::size_t size) { return guardedAllocate(size, __builtin_return_address(0), false); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return guardedAllocate(size, __builtin_return_address(0), true); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return guardedAllocate(size, __builtin_return_address(0), true); }
void operator delete(void* pointer) noexcept { guardedFree(pointer, __builtin_return_address(0), static_cast<std::size_t>(-1)); }
void operator delete[](void* pointer) noexcept { guardedFree(pointer, __builtin_return_address(0), static_cast<std::size_t>(-1)); }
void operator delete(void* pointer, std::size_t size) noexcept { guardedFree(pointer, __builtin_return_address(0), size); }
void operator delete[](void* pointer, std::size_t size) noexcept { guardedFree(pointer, __builtin_return_address(0), size); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { guardedFree(pointer, __builtin_return_address(0), static_cast<std::size_t>(-1)); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { guardedFree(pointer, __builtin_return_address(0), static_cast<std::size_t>(-1)); }
