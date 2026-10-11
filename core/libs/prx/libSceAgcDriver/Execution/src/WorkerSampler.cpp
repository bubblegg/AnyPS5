#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

extern "C" char __ImageBase;
#endif

namespace AgcDriver {

#ifdef _WIN32
namespace {

std::jthread processSamplerThread;
std::jthread workerSamplerThread;
std::jthread committerSamplerThread;

// Debug aid: APS5_SAMPLE_WORKER=<file> samples the calling thread every millisecond and, every 20 s,
// writes "module+offset self inclusive" lines to <file> for offline symbolization with nm, and the
// most frequent whole call chains (leaf first) to <file>.stacks.
struct Sampler {
    HANDLE target = nullptr;
    std::string path;
    std::mutex mutex;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> counts;
    std::uint64_t samples = 0;
    int depth = 12;
    bool chains = false;
    // APS5_SAMPLE_WINDOWS=1: every write goes to its own file (<file>.0, <file>.1, ...) and starts
    // the counts again, so one scene can be read apart from the menus and loads around it.
    bool windows = false;
    int window = 0;
    std::map<std::vector<std::uint64_t>, std::uint64_t> stacks;

    ~Sampler() noexcept(false) {
        if (target != nullptr && !CloseHandle(target)) throw std::system_error(GetLastError(), std::system_category(), "Closing sampler thread handle");
    }

    static constexpr int MaxDepth = 128;
    static constexpr std::size_t StackWords = 4096;
    std::vector<std::uint64_t> stackWords = std::vector<std::uint64_t>(StackWords);

    void sample() {
        // Nothing may allocate while the target is suspended: it may hold the heap lock (the draw
        // thread spends much of its time in malloc), and the sampler would then wait for good.
        std::array<std::uint64_t, MaxDepth> walked{};
        std::size_t walkedCount = 0, stackCount = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return;
        const bool captured = GetThreadContext(target, &context) != 0;
        if (captured) {
            // Unwinding reads the target's stack while it is suspended; frames without unwind data
            // (JIT/guest code) end the walk.
            for (int frame = 0; frame < std::min(depth, MaxDepth) && context.Rip != 0; ++frame) {
                walked[walkedCount++] = context.Rip;
                DWORD64 imageBase = 0;
                auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (entry == nullptr) break;
                PVOID handler = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handler, &establisher, nullptr);
            }
            // Most of this library has only DWARF unwind data, which ends the walk at its first
            // frame. For call chains, the stack above that point is copied and scanned for return
            // addresses into this library afterwards.
            if (chains && context.Rsp != 0) stackCount = copyStack(context.Rsp, stackWords);
        }
        ResumeThread(target);
        std::vector<std::uint64_t> frames(walked.begin(), walked.begin() + static_cast<std::ptrdiff_t>(walkedCount));
        if (stackCount != 0) scanReturnAddresses(std::span<const std::uint64_t>(stackWords).first(stackCount), frames);
        std::lock_guard lock(mutex);
        ++samples;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (frames[i] == 0) break;
            auto& count = counts[frames[i]];
            if (i == 0) ++count.first;
            ++count.second;
        }
        if (chains && !frames.empty()) ++stacks[frames];
    }

    // Copies the stack from rsp into words without allocating; returns the words copied.
    static std::size_t copyStack(std::uint64_t rsp, std::vector<std::uint64_t>& words) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(rsp), &region, sizeof(region)) == 0 || region.State != MEM_COMMIT) return 0;
        const auto end = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
        const auto bytes = std::min<std::uint64_t>(end - rsp, words.size() * 8) & ~std::uint64_t{7};
        SIZE_T read = 0;
        if (bytes == 0 || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(rsp), words.data(), bytes, &read)) return 0;
        return read / 8;
    }

    // Appends 0 (a marker: the frames after it are guessed) and every stack word that points into
    // this library's code just after a call instruction, nearest first.
    static void scanReturnAddresses(std::span<const std::uint64_t> stack, std::vector<std::uint64_t>& frames) {
        static const auto [begin, end] = [] {
            const auto base = reinterpret_cast<std::uint64_t>(&__ImageBase);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            return std::pair<std::uint64_t, std::uint64_t>{base + nt->OptionalHeader.BaseOfCode, base + nt->OptionalHeader.BaseOfCode + nt->OptionalHeader.SizeOfCode};
        }();
        const auto calls = [](std::uint64_t address) {
            const auto* p = reinterpret_cast<const unsigned char*>(address);
            const auto indirect = [&](int at) { return p[-at] == 0xff && (p[-at + 1] & 0x38u) == 0x10u; };
            return p[-5] == 0xe8 || indirect(2) || indirect(3) || indirect(6) || indirect(7);
        };
        frames.push_back(0);
        std::size_t found = 0;
        for (const auto word : stack) {
            if (word < begin + 8 || word >= end || !calls(word)) continue;
            frames.push_back(word);
            if (++found >= 48) break;
        }
    }

    static std::string frameName(std::uint64_t address) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
        const char* base = std::strrchr(name, '\\');
        char text[MAX_PATH + 32];
        std::snprintf(text, sizeof(text), "%s+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)));
        return text;
    }

    // Called with the mutex held: the 2000 most frequent chains as "count frame;frame;..." lines.
    void writeStacks(const std::string& output) {
        std::FILE* file = std::fopen((output + ".stacks").c_str(), "w");
        if (file == nullptr) return;
        std::vector<std::pair<std::uint64_t, const std::vector<std::uint64_t>*>> order;
        order.reserve(stacks.size());
        for (const auto& [frames, count] : stacks) order.emplace_back(count, &frames);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::map<std::uint64_t, std::string> names;
        std::fprintf(file, "# %llu samples, %zu distinct chains\n", static_cast<unsigned long long>(samples), stacks.size());
        for (std::size_t i = 0; i < order.size() && i < 2000; ++i) {
            std::fprintf(file, "%llu ", static_cast<unsigned long long>(order[i].first));
            const auto& frames = *order[i].second;
            for (std::size_t f = 0; f < frames.size(); ++f) {
                auto [it, inserted] = names.try_emplace(frames[f]);
                if (inserted) it->second = frames[f] == 0 ? "~" : frameName(frames[f]);
                std::fprintf(file, f == 0 ? "%s" : ";%s", it->second.c_str());
            }
            std::fprintf(file, "\n");
        }
        std::fclose(file);
    }

    void write() {
        std::lock_guard lock(mutex);
        const std::string output = windows ? path + "." + std::to_string(window++) : path;
        std::FILE* file = std::fopen(output.c_str(), "w");
        if (file == nullptr) return;
        std::fprintf(file, "# %llu samples\n", static_cast<unsigned long long>(samples));
        for (const auto& [address, count] : counts) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
            const char* base = std::strrchr(name, '\\');
            std::fprintf(file, "%s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(count.first), static_cast<unsigned long long>(count.second));
        }
        std::fclose(file);
        if (chains) writeStacks(output);
        if (windows) {
            counts.clear();
            stacks.clear();
            samples = 0;
        }
    }
};

// Debug aid: APS5_SAMPLE_THREADS=<file> samples every thread in the process every 2 ms and, every
// 20 s, writes each busy thread's hottest frames ("module+offset self inclusive") to <file>.
struct ProcessSampler {
    std::string path;
    DWORD self = 0;
    std::map<DWORD, Sampler> threads;
    std::uint64_t rounds = 0;

    std::chrono::steady_clock::time_point enumerated{};

    // The system-wide thread snapshot is slow (tens of ms); the thread list is refreshed every 5 s and
    // the rounds in between only sample the known threads.
    void enumerate() {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            auto& thread = threads[entry.th32ThreadID];
            if (thread.target == nullptr) thread.target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        }
        CloseHandle(snapshot);
        enumerated = std::chrono::steady_clock::now();
    }

    void sample() {
        if (threads.empty() || std::chrono::steady_clock::now() - enumerated > std::chrono::seconds(5)) enumerate();
        for (auto& [id, thread] : threads) {
            if (thread.target != nullptr) thread.sample();
        }
        ++rounds;
    }

    void write() {
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) return;
        std::fprintf(file, "# %llu rounds\n", static_cast<unsigned long long>(rounds));
        for (auto& [id, thread] : threads) {
            std::lock_guard lock(thread.mutex);
            std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>> hot(thread.counts.begin(), thread.counts.end());
            std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            std::uint64_t leaves = 0;
            for (const auto& [address, count] : hot) leaves += count.first;
            if (leaves == 0) continue;
            std::fprintf(file, "thread %lu samples %llu\n", static_cast<unsigned long>(id), static_cast<unsigned long long>(thread.samples));
            for (std::size_t i = 0; i < hot.size() && i < 40; ++i) {
                const auto address = hot[i].first;
                HMODULE module = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
                const char* base = std::strrchr(name, '\\');
                std::fprintf(file, "  %s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(hot[i].second.first), static_cast<unsigned long long>(hot[i].second.second));
            }
        }
        std::fclose(file);
    }
};

void StartProcessSampler() {
    const char* path = std::getenv("APS5_SAMPLE_THREADS");
    if (path == nullptr) return;
    processSamplerThread = std::jthread([path = std::string(path)](std::stop_token token) {
        auto sampler = std::make_unique<ProcessSampler>();
        sampler->path = path;
        sampler->self = GetCurrentThreadId();
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

}

namespace {

void startSampler(const char* path, std::jthread& thread);

}

void StartWorkerSampler() {
    StartProcessSampler();
    startSampler(std::getenv("APS5_SAMPLE_WORKER"), workerSamplerThread);
}

void StartCommitterSampler() {
    startSampler(std::getenv("APS5_SAMPLE_COMMITTER"), committerSamplerThread);
}

namespace {

void startSampler(const char* path, std::jthread& thread) {
    if (path == nullptr || thread.joinable()) return;
    auto sampler = std::make_unique<Sampler>();
    sampler->path = path;
    // Deep enough to reach the driver from inside the Vulkan driver's compiler and from host waits.
    sampler->depth = 96;
    sampler->chains = true;
    sampler->windows = std::getenv("APS5_SAMPLE_WINDOWS") != nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampler->target, THREAD_ALL_ACCESS, FALSE, 0)) throw std::system_error(GetLastError(), std::system_category(), "Duplicating sampler thread handle");
    thread = std::jthread([sampler = std::move(sampler)](std::stop_token token) {
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

}

void StopWorkerSampler() {
    processSamplerThread.request_stop();
    workerSamplerThread.request_stop();
    committerSamplerThread.request_stop();
    if (committerSamplerThread.joinable()) committerSamplerThread.join();
    if (processSamplerThread.joinable()) processSamplerThread.join();
    if (workerSamplerThread.joinable()) workerSamplerThread.join();
}
#else
void StartWorkerSampler() {}
void StartCommitterSampler() {}
void StopWorkerSampler() {}
#endif

}
