#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
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

namespace {

std::mutex& liveMutex() {
    static std::mutex mutex;
    return mutex;
}

std::set<const ShaderRegistry*>& liveRegistries() {
    static auto* live = new std::set<const ShaderRegistry*>();
    return *live;
}

std::atomic<std::uint64_t> registriesMade{0};
std::atomic<std::uint64_t> audits{0};

}

ShaderRegistry::ShaderRegistry() {
    std::lock_guard lock(liveMutex());
    liveRegistries().insert(this);
    ++registriesMade;
}

ShaderRegistry::ShaderRegistry(const ShaderRegistry& other) : std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>>(other) {
    std::lock_guard lock(liveMutex());
    liveRegistries().insert(this);
    ++registriesMade;
}

ShaderRegistry::~ShaderRegistry() {
    std::lock_guard lock(liveMutex());
    liveRegistries().erase(this);
}

void AuditShaderRegistries(const ShaderRegistry& current, const char* when) {
    std::lock_guard lock(liveMutex());
    ++audits;
    const auto& live = liveRegistries();
    std::size_t low = 0;
    for (const auto& [address, snapshot] : current) {
        long holders = 0;
        for (const auto* registry : live) {
            const auto found = registry->find(address);
            if (found != registry->end() && found->second == snapshot) ++holders;
        }
        const long count = snapshot.use_count();
        if (count >= holders) continue;
        if (low++ >= 6) continue;
        std::fprintf(stderr, "[audit] %s: snapshot %p (code 0x%llx, type %u) has use count %ld but %ld live registries hold it\n", when, static_cast<const void*>(snapshot.get()), static_cast<unsigned long long>(address), static_cast<unsigned>(snapshot->type), count, holders);
#ifdef _WIN32
        // The control block of a make_shared object sits 16 bytes before it: vtable pointer, use count, weak count.
        const auto* raw = reinterpret_cast<const std::uint64_t*>(snapshot.get()) - 6;
        std::fprintf(stderr, "[audit]   memory from 32 bytes before its control block:");
        for (int i = 0; i < 14; ++i) std::fprintf(stderr, " %016llx", static_cast<unsigned long long>(raw[i]));
        std::fprintf(stderr, "\n[audit]   code address field 0x%llx, header address 0x%llx, code words %zu, header bytes %zu\n", static_cast<unsigned long long>(snapshot->codeAddress), static_cast<unsigned long long>(snapshot->headerAddress), snapshot->code.size(), snapshot->header.size());
        const auto& wire = snapshot->tripwire;
        if (wire.state == Destroyed) printFrames("it was already destroyed; that destruction", wire.firstThread, wire.first, wire.firstCount < DestructionTripwire::FirstCapacity ? wire.firstCount : DestructionTripwire::FirstCapacity);
        else std::fprintf(stderr, "[audit]   it has not been destroyed (marker 0x%llx): its count was changed without a release reaching zero\n", static_cast<unsigned long long>(wire.state));
#endif
    }
    if (low == 0) return;
    std::fprintf(stderr, "[audit] %s: %zu of %zu snapshots are short of references; %zu live registries, %llu made, audit %llu, thread %lu\n", when, low, current.size(), live.size(), static_cast<unsigned long long>(registriesMade.load()), static_cast<unsigned long long>(audits.load()),
#ifdef _WIN32
                 static_cast<unsigned long>(GetCurrentThreadId())
#else
                 0ul
#endif
    );
    std::fflush(stderr);
    std::abort();
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
        std::fprintf(stderr, "[snapshot] a %s at %p is destroyed a second time\n", self->name, static_cast<void*>(this));
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
