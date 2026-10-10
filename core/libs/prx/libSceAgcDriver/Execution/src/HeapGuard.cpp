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
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

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

void* guardedAllocate(std::size_t size, void* site, bool nothrow) {
    const bool guard = guardEnabled();
    auto* block = static_cast<unsigned char*>(std::malloc(guard ? size + TailBytes : size != 0 ? size : 1));
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
