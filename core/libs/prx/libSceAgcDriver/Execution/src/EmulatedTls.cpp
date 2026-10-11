// This toolchain (MinGW-w64 GCC, POSIX threads) has no native thread-local storage: every access to
// a thread_local calls __emutls_get_address, and libgcc's goes through winpthreads'
// pthread_getspecific, which takes a spin lock and looks the thread up. The draw path reads dozens of
// thread_locals per draw, and that call was about a quarter of the preparing thread's time.
//
// The library is linked with --wrap=__emutls_get_address, so its accesses land here instead. A
// variable of this library gets an index into a per-thread table that hangs off one Windows TLS
// slot, read straight from the thread's environment block. A variable that lives in another module
// (the C++ runtime library's, which that library reads through libgcc) still goes to libgcc, so
// both sides see the same storage.
//
// A thread's table and variables are never freed: destructors of thread_locals run from the C++
// runtime's own thread-exit hook, which may come after any hook this file could register, and a
// thread touches a few kilobytes at most.
#ifdef _WIN32
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

extern "C" char __ImageBase;

namespace {

// libgcc's descriptor of one thread_local (emutls.c); `index` is 0 until the variable is first used.
struct EmutlsObject {
    std::size_t size;
    std::size_t align;
    std::uintptr_t index;
    void* initial;
};

struct ThreadTable {
    std::size_t count;
    void* values[1];
};

constexpr std::size_t TebTlsSlots = 0x1480;
constexpr std::size_t TebTlsExpansionSlots = 0x1780;
constexpr DWORD DirectSlots = 64;

// 0 until the slot is allocated; never set if it could not be, and libgcc then serves everything.
std::atomic<std::uintptr_t> imageBytes{0};
DWORD tableSlot = TLS_OUT_OF_INDEXES;
std::atomic<bool> unavailable{false};
SRWLOCK setupLock = SRWLOCK_INIT;
std::uintptr_t nextIndex = 0;

inline ThreadTable* currentTable() {
    auto* teb = reinterpret_cast<unsigned char*>(NtCurrentTeb());
    if (tableSlot < DirectSlots) return static_cast<ThreadTable*>(reinterpret_cast<void**>(teb + TebTlsSlots)[tableSlot]);
    auto** expansion = *reinterpret_cast<void***>(teb + TebTlsExpansionSlots);
    return expansion != nullptr ? static_cast<ThreadTable*>(expansion[tableSlot - DirectSlots]) : nullptr;
}

inline bool ownObject(const EmutlsObject* object, std::uintptr_t bytes) {
    return reinterpret_cast<std::uintptr_t>(object) - reinterpret_cast<std::uintptr_t>(&__ImageBase) < bytes;
}

bool setUp() {
    if (imageBytes.load(std::memory_order_acquire) != 0) return true;
    if (unavailable.load(std::memory_order_acquire)) return false;
    AcquireSRWLockExclusive(&setupLock);
    if (imageBytes.load(std::memory_order_relaxed) == 0 && !unavailable.load(std::memory_order_relaxed)) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const unsigned char*>(dos) + dos->e_lfanew);
        tableSlot = TlsAlloc();
        if (tableSlot == TLS_OUT_OF_INDEXES) unavailable.store(true, std::memory_order_release);
        else imageBytes.store(nt->OptionalHeader.SizeOfImage, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&setupLock);
    return imageBytes.load(std::memory_order_acquire) != 0;
}

}

extern "C" void* __real___emutls_get_address(void* object);

namespace {

// The first use of a variable on a thread (or of anything before the slot exists).
[[gnu::noinline]] void* slowAddress(EmutlsObject* object) {
    const DWORD lastError = GetLastError();
    if (!setUp() || !ownObject(object, imageBytes.load(std::memory_order_relaxed))) {
        SetLastError(lastError);
        return __real___emutls_get_address(object);
    }
    auto index = __atomic_load_n(&object->index, __ATOMIC_ACQUIRE);
    if (index == 0) {
        AcquireSRWLockExclusive(&setupLock);
        index = object->index;
        if (index == 0) {
            index = ++nextIndex;
            __atomic_store_n(&object->index, index, __ATOMIC_RELEASE);
        }
        ReleaseSRWLockExclusive(&setupLock);
    }
    const HANDLE heap = GetProcessHeap();
    auto* table = currentTable();
    if (table == nullptr || table->count < index) {
        const std::size_t count = index + 64;
        const std::size_t bytes = offsetof(ThreadTable, values) + count * sizeof(void*);
        auto* grown = static_cast<ThreadTable*>(table == nullptr ? HeapAlloc(heap, HEAP_ZERO_MEMORY, bytes) : HeapReAlloc(heap, HEAP_ZERO_MEMORY, table, bytes));
        if (grown == nullptr || !TlsSetValue(tableSlot, grown)) std::abort();
        grown->count = count;
        table = grown;
    }
    const std::size_t align = object->align > 16 ? object->align : 16;
    auto* raw = static_cast<unsigned char*>(HeapAlloc(heap, 0, object->size + align));
    if (raw == nullptr) std::abort();
    auto* value = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(raw) + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1));
    if (object->initial != nullptr) std::memcpy(value, object->initial, object->size);
    else std::memset(value, 0, object->size);
    table->values[index - 1] = value;
    SetLastError(lastError);
    return value;
}

}

extern "C" void* __wrap___emutls_get_address(void* descriptor) {
    auto* object = static_cast<EmutlsObject*>(descriptor);
    if (ownObject(object, imageBytes.load(std::memory_order_acquire))) [[likely]] {
        const auto index = __atomic_load_n(&object->index, __ATOMIC_ACQUIRE);
        const auto* table = currentTable();
        if (index != 0 && table != nullptr && index <= table->count) [[likely]] {
            if (void* value = table->values[index - 1]) [[likely]] return value;
        }
    }
    return slowAddress(object);
}
#endif
