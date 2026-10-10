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
void printFrames(const char* title, unsigned thread, void* const* frames, unsigned count) {
    std::fprintf(stderr, "[snapshot] %s (thread %u):\n", title, thread);
    for (unsigned i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &module) && module != nullptr) GetModuleFileNameA(module, path, sizeof(path));
        const char* leaf = std::strrchr(path, '\\');
        std::fprintf(stderr, "[snapshot]   #%u %s+0x%llx\n", i, leaf != nullptr ? leaf + 1 : path, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module)));
    }
}
#endif

}

DestructionTripwire::~DestructionTripwire() {
#ifdef _WIN32
    if (state == Alive) {
        state = Destroyed;
        firstCount = CaptureStackBackTrace(0, 20, first, nullptr);
        firstThread = GetCurrentThreadId();
        return;
    }
    void* second[20] = {};
    const unsigned secondCount = CaptureStackBackTrace(0, 20, second, nullptr);
    if (state == Destroyed) {
        std::fprintf(stderr, "[snapshot] a shader snapshot at %p is destroyed a second time\n", static_cast<void*>(this));
        printFrames("first destruction", firstThread, first, firstCount < 20 ? firstCount : 20);
    } else {
        std::fprintf(stderr, "[snapshot] a shader snapshot at %p is destroyed after its memory was reused (marker 0x%llx)\n", static_cast<void*>(this), static_cast<unsigned long long>(state));
    }
    printFrames("this destruction", GetCurrentThreadId(), second, secondCount);
    std::fflush(stderr);
    std::abort();
#endif
}

}
