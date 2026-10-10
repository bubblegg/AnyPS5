// Debug aid: APS5_GUARD_NEW=1 appends a canary, the allocating address and a signature to every
// block this library allocates with operator new, and checks the canary when the block is deleted
// here. A trampled canary stops the process with the block's size and the address (as an offset
// into this library) of the code that allocated it: the owner of the buffer that was overrun.
// The block's address is malloc's own, so the C++ runtime library may free it as usual; a block
// without the signature (allocated elsewhere, or overrun past the whole tail) is freed unchecked.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

extern "C" char __ImageBase;

namespace {

constexpr std::uint64_t Signature = 0x4150533547554152ull;
constexpr std::size_t CanaryBytes = 16;
constexpr std::size_t TailBytes = CanaryBytes + 16;
constexpr unsigned char CanaryByte = 0xa5;

bool guardEnabled() {
    static const bool enabled = std::getenv("APS5_GUARD_NEW") != nullptr;
    return enabled;
}

void* guardedAllocate(std::size_t size, void* site, bool nothrow) {
    const bool guard = guardEnabled();
    auto* block = static_cast<unsigned char*>(std::malloc(guard ? size + TailBytes : size != 0 ? size : 1));
    if (block == nullptr) {
        if (nothrow) return nullptr;
        throw std::bad_alloc();
    }
    if (guard) {
        const std::uint64_t signature = Signature ^ reinterpret_cast<std::uintptr_t>(block) ^ (static_cast<std::uint64_t>(size) << 20u);
        std::memset(block + size, CanaryByte, CanaryBytes);
        std::memcpy(block + size + CanaryBytes, &site, sizeof(site));
        std::memcpy(block + size + CanaryBytes + 8, &signature, sizeof(signature));
    }
    return block;
}

__attribute__((always_inline)) inline void guardedFree(void* pointer, std::size_t deleting = static_cast<std::size_t>(-1)) noexcept {
    if (pointer == nullptr) return;
    if (guardEnabled()) {
        auto* block = static_cast<unsigned char*>(pointer);
        const std::size_t total = _msize(pointer);
        if (total >= TailBytes && total != static_cast<std::size_t>(-1)) {
            const std::size_t size = total - TailBytes;
            std::uint64_t signature = 0;
            std::memcpy(&signature, block + size + CanaryBytes + 8, sizeof(signature));
            if (signature == (Signature ^ reinterpret_cast<std::uintptr_t>(block) ^ (static_cast<std::uint64_t>(size) << 20u))) {
                // A sized delete names the size of the object it destroys: another size means this block
                // belongs to a different owner now, so the caller is deleting something a second time.
                if (deleting != static_cast<std::size_t>(-1) && deleting != size) {
                    void* owner = nullptr;
                    std::memcpy(&owner, block + size + CanaryBytes, sizeof(owner));
                    const auto image = reinterpret_cast<std::uintptr_t>(&__ImageBase);
                    std::fprintf(stderr, "[heap-guard] a delete of %llu bytes at libSceAgcDriver.prx+0x%llx hit a live block of %llu bytes that was allocated at +0x%llx: the deleted object was already freed once\n", static_cast<unsigned long long>(deleting), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - image), static_cast<unsigned long long>(size), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(owner) - image));
                    std::fflush(stderr);
                    std::abort();
                }
                for (std::size_t i = 0; i < CanaryBytes; ++i) {
                    if (block[size + i] == CanaryByte) continue;
                    void* site = nullptr;
                    std::memcpy(&site, block + size + CanaryBytes, sizeof(site));
                    const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
                    std::fprintf(stderr, "[heap-guard] a block of %llu bytes was overrun: allocated at libSceAgcDriver.prx+0x%llx, deleted at +0x%llx; the %zu bytes after it:", static_cast<unsigned long long>(size), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(site) - base), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - base), CanaryBytes);
                    for (std::size_t b = 0; b < CanaryBytes; ++b) std::fprintf(stderr, " %02x", block[size + b]);
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                    std::abort();
                }
                // A second delete of the block must not pass as checked.
                std::memset(block + size + CanaryBytes + 8, 0, 8);
            }
        }
    }
    std::free(pointer);
}

}

void* operator new(std::size_t size) { return guardedAllocate(size, __builtin_return_address(0), false); }
void* operator new[](std::size_t size) { return guardedAllocate(size, __builtin_return_address(0), false); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return guardedAllocate(size, __builtin_return_address(0), true); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return guardedAllocate(size, __builtin_return_address(0), true); }
void operator delete(void* pointer) noexcept { guardedFree(pointer); }
void operator delete[](void* pointer) noexcept { guardedFree(pointer); }
void operator delete(void* pointer, std::size_t size) noexcept { guardedFree(pointer, size); }
void operator delete[](void* pointer, std::size_t size) noexcept { guardedFree(pointer, size); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { guardedFree(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { guardedFree(pointer); }
