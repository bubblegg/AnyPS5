#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint64_t Alive = 0x534e4150414c4956ull;
constexpr std::uint64_t Destroyed = 0x534e415044454144ull;

#ifdef _WIN32
// The driver image's address range. Frame-pointer walks stop at once in this build, so the stack is
// scanned for values that point into the driver instead: return addresses, with some stale ones.
void driverRange(std::uintptr_t& low, std::uintptr_t& high) {
    static std::uintptr_t cachedLow = 0, cachedHigh = 0;
    if (cachedLow == 0) {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&driverRange), &module);
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        cachedHigh = base + nt->OptionalHeader.SizeOfImage;
        cachedLow = base;
    }
    low = cachedLow;
    high = cachedHigh;
}

unsigned scanStack(void** out, unsigned capacity) {
    std::uintptr_t low, high;
    driverRange(low, high);
    void* marker = nullptr;
    auto* word = reinterpret_cast<const std::uintptr_t*>(&marker);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(word, &info, sizeof(info)) != sizeof(info)) return 0;
    auto* end = reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
    if (end - word > 6000) end = word + 6000;
    unsigned count = 0;
    std::uintptr_t previous = 0;
    for (; word < end && count < capacity; ++word) {
        const auto value = *word;
        if (value < low + 0x1000 || value >= high || value == previous) continue;
        previous = value;
        out[count++] = reinterpret_cast<void*>(value);
    }
    return count;
}

void printFrames(const char* title, unsigned thread, void* const* frames, unsigned count) {
    std::uintptr_t low, high;
    driverRange(low, high);
    std::fprintf(stderr, "[snapshot] %s (thread %u), driver addresses on its stack, innermost first:\n[snapshot]  ", title, thread);
    for (unsigned i = 0; i < count; ++i) std::fprintf(stderr, " +0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]) - low));
    std::fprintf(stderr, "\n");
}
#endif

}

DestructionTripwire::~DestructionTripwire() {
#ifdef _WIN32
    // Stores into an object that is being destroyed are dead to the compiler; go through a volatile pointer.
    DestructionTripwire* volatile self = this;
    if (self->state == Alive) {
        self->state = Destroyed;
        self->firstCount = scanStack(self->first, FirstCapacity);
        self->firstThread = GetCurrentThreadId();
        return;
    }
    void* second[FirstCapacity] = {};
    const unsigned secondCount = scanStack(second, FirstCapacity);
    if (self->state == Destroyed) {
        std::fprintf(stderr, "[snapshot] a shader snapshot at %p is destroyed a second time\n", static_cast<void*>(this));
        printFrames("first destruction", self->firstThread, self->first, self->firstCount < FirstCapacity ? self->firstCount : FirstCapacity);
    } else {
        std::fprintf(stderr, "[snapshot] a shader snapshot at %p is destroyed after its memory was reused (marker 0x%llx)\n", static_cast<void*>(this), static_cast<unsigned long long>(self->state));
    }
    printFrames("this destruction", GetCurrentThreadId(), second, secondCount);
    std::fflush(stderr);
    std::abort();
#endif
}

}
