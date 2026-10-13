#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace AgcDriver {
namespace {

// Calls run(first, end) for each run of set bits, a 64-bit word at a time (a page is scanned for
// every stage of every draw; testing it bit by bit cost the draw path several percent).
template<std::size_t Bits, typename TRun>
void forEachRun(const std::bitset<Bits>& bits, TRun&& run) {
    if (bits.none()) return;
    const auto clear = ~bits;
    for (std::size_t first = bits._Find_first(); first < Bits;) {
        const std::size_t end = clear._Find_next(first);
        run(first, end);
        if (end >= Bits) return;
        first = bits._Find_next(end);
    }
}

struct CaptureProfile {
    std::atomic<std::uint64_t> captures{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> pages{0};
    // The whole CaptureResources call: the plan lookup and the materialization are one call.
    std::atomic<std::uint64_t> captureNanoseconds{0};
    // Pages read word by word because recorded GPU work writes them, and the word reads of those
    // pages that waited (a read taking longer than WordWaitNanoseconds went through a hook sync),
    // were read raw on the driver's evidence, or were read both ways (verify) and differed.
    std::atomic<std::uint64_t> pagesWordwise{0};
    std::atomic<std::uint64_t> wordWaits{0};
    std::atomic<std::uint64_t> wordWaitNanoseconds{0};
    std::atomic<std::uint64_t> wordsRaw{0};
    std::atomic<std::uint64_t> wordsVerified{0};
    std::atomic<std::uint64_t> wordMismatches{0};
    // Words served from the driver's known values (never profile-gated: the [copy] line reads them).
    std::atomic<std::uint64_t> wordsKnown{0};
    std::atomic<std::uint64_t> wordsKnownVerified{0};
    std::atomic<std::uint64_t> wordsKnownMismatches{0};
    // Hook reads not reported to the observer because no GPU wait happened across them.
    std::atomic<std::uint64_t> observationsSkipped{0};
    // Sub-phases of a capture: the source lookup (the stage inputs, the recompiler's key over the
    // code and the plan, timed by CaptureResources itself: ResourceCapture::sourceNanoseconds),
    // whole-page fetches, per-word fetches (their hook waits included) and the GPU waits the flush
    // hook made anywhere inside the capture (Recorder::ThreadWaitedMs). The walk itself is what
    // remains of the capture time after the lookup, the fetches and the specialization
    // (ResourceMaterializer).
    std::atomic<std::uint64_t> resolveNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> wordReadNanoseconds{0};
    std::atomic<std::uint64_t> hookWaitNanoseconds{0};
    // The driver's per-shader source handle memo: captures served from it, and resolves it made.
    std::atomic<std::uint64_t> handleHits{0};
    std::atomic<std::uint64_t> handleMisses{0};
};

constexpr std::uint64_t WordWaitNanoseconds = 50000;

bool WordwisePages() {
    static const bool wordwise = std::getenv("APS5_NO_WORDWISE_CAPTURE") == nullptr;
    return wordwise;
}

bool CaptureProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::uint64_t NanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

CaptureProfile& CaptureTotals() {
    static CaptureProfile profile;
    return profile;
}

std::atomic<ShaderMemory::WaitedMsProvider> waitedMsProvider{nullptr};

double WaitedMs() {
    const auto provider = waitedMsProvider.load(std::memory_order_acquire);
    return provider != nullptr ? provider() : 0.0;
}

}

void ShaderMemory::SetWaitedMsProvider(WaitedMsProvider provider) {
    waitedMsProvider.store(provider, std::memory_order_release);
}

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> regions, PendingWriteQuery pendingWrite, PendingWriteObserver observe, HookWaitCounter hookWaits) {
    Reset(regions, pendingWrite, observe, hookWaits);
}

void ShaderMemory::Reset(std::span<const ShaderRecompiler::MemoryRegion> regions, PendingWriteQuery pendingWriteQuery, PendingWriteObserver observer, HookWaitCounter hookWaitCounter) {
    pendingWrite = pendingWriteQuery;
    observe = observer;
    hookWaits = hookWaitCounter;
    pagesUsed = 0;
    initial.clear();
    const ShaderRecompiler::RequestMemoryView validated(regions);
    for (const auto& region : regions) initial.emplace_back(region.guestAddress, region.bytes);
    // The first region given at an address is the one kept.
    std::stable_sort(initial.begin(), initial.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    initial.erase(std::unique(initial.begin(), initial.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), initial.end());
    initialLow = initial.empty() ? 0 : initial.front().first;
    initialHigh = 0;
    for (const auto& [address, bytes] : initial) initialHigh = std::max<std::uint64_t>(initialHigh, address + bytes.size());
    lastBase = ~std::uint64_t{0};
    lastPage = nullptr;
}

ShaderMemory::InitialRegions::const_iterator ShaderMemory::initialAfter(std::uint64_t address) const {
    return std::upper_bound(initial.begin(), initial.end(), address, [](std::uint64_t value, const auto& region) { return value < region.first; });
}

namespace {

constexpr std::size_t PooledPages = 4096;

struct PagePool {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<void*> pages;
};

// Never destroyed: pages are returned by whichever thread ends a draw, at any time.
PagePool& pagePool() {
    static auto* pool = new PagePool();
    return *pool;
}

}

ShaderMemory::PagePointer ShaderMemory::takePage() {
    auto& pool = pagePool();
    Page* page = nullptr;
    while (pool.lock.test_and_set(std::memory_order_acquire)) {
    }
    if (!pool.pages.empty()) {
        page = static_cast<Page*>(pool.pages.back());
        pool.pages.pop_back();
    }
    pool.lock.clear(std::memory_order_release);
    if (page == nullptr) return PagePointer(new Page);
    page->valid.reset();
    page->read.reset();
    page->recent.reset();
    page->wordwise = false;
    page->whole = false;
    return PagePointer(page);
}

void ShaderMemory::PageRelease::operator()(Page* page) const noexcept {
    auto& pool = pagePool();
    while (pool.lock.test_and_set(std::memory_order_acquire)) {
    }
    const bool kept = pool.pages.size() < PooledPages;
    if (kept) pool.pages.push_back(page);
    pool.lock.clear(std::memory_order_release);
    if (!kept) delete page;
}

ShaderMemory::Page& ShaderMemory::page(std::uint64_t base) {
    if (base == lastBase) return *lastPage;
    for (std::size_t i = 0; i < pagesUsed; ++i) {
        if (pages[i].first == base) {
            lastBase = base;
            lastPage = pages[i].second.get();
            return *lastPage;
        }
    }
    if (pagesUsed == pages.size()) {
        pages.emplace_back(base, takePage());
    } else {
        auto& kept = *pages[pagesUsed].second;
        kept.valid.reset();
        kept.read.reset();
        kept.recent.reset();
        kept.wordwise = false;
        kept.whole = false;
        pages[pagesUsed].first = base;
    }
    auto at = pagesUsed++;
    for (; at > 0 && pages[at - 1].first > base; --at) std::swap(pages[at - 1], pages[at]);
    auto& page = *pages[at].second;
    lastBase = base;
    lastPage = &page;
    if (CaptureProfiled()) ++CaptureTotals().pages;
    // A page is either mapped whole or read word by word where the guest mapped less than a page,
    // or where recorded GPU work writes into it (a whole-page read would wait for that work).
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) {
        if (WordwisePages() && pendingWrite != nullptr && pendingWrite(base, PageBytes, {}) != PendingWrite::None) {
            page.wordwise = true;
            if (CaptureProfiled()) ++CaptureTotals().pagesWordwise;
            return page;
        }
        const auto started = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        GuestMemory::Read(base, std::as_writable_bytes(std::span(page.words)), sizeof(std::uint32_t));
        if (CaptureProfiled()) CaptureTotals().pageReadNanoseconds += NanosecondsSince(started);
        page.valid.set();
        page.whole = true;
    }
    return page;
}

bool ShaderMemory::read(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        throw std::runtime_error("AGC driver: invalid shader memory read address");
    }
    if (CaptureProfiled()) ++CaptureTotals().reads;
    if (address < self.initialHigh && address + sizeof(*value) > self.initialLow) {
        const auto next = self.initialAfter(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            const auto offset = address - previous->first;
            if (offset < previous->second.size()) {
                if (previous->second.size() - offset < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read crosses a snapshot boundary");
                std::memcpy(value, previous->second.data() + offset, sizeof(*value));
                return true;
            }
        }
        if (next != self.initial.end() && next->first - address < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read overlaps a snapshot boundary");
    }
    auto& page = self.page(address & ~static_cast<std::uint64_t>(PageBytes - 1));
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    if (!page.valid.test(index)) {
        std::uint32_t word = 0;
        auto policy = PendingWrite::Sync;
        if (page.wordwise) {
            policy = self.pendingWrite(address, sizeof(word), std::as_writable_bytes(std::span(&word, 1)));
            if (policy != PendingWrite::None && policy != PendingWrite::Sync && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word))) policy = PendingWrite::Sync;
        }
        // A read is evidence only when the hook waited for unfinished GPU work across it.
        const auto hookWaits = [&] { return self.hookWaits != nullptr ? self.hookWaits() : 0; };
        const auto report = [&](std::uint64_t waitsBefore, bool unchanged) {
            if (self.hookWaits == nullptr || self.hookWaits() != waitsBefore) self.observe(address, unchanged);
            else ++CaptureTotals().observationsSkipped;
        };
        const auto fetchStarted = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (policy == PendingWrite::KnownValue || policy == PendingWrite::VerifyKnownValue) {
            // The query stored the known dword in `word`.
            ++CaptureTotals().wordsKnown;
            if (policy == PendingWrite::VerifyKnownValue) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsKnownVerified;
                if (waited != word) ++CaptureTotals().wordsKnownMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word);
                word = waited;
            }
        } else if (policy == PendingWrite::Raw || policy == PendingWrite::VerifyRaw) {
            std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            ++CaptureTotals().wordsRaw;
            if (policy == PendingWrite::VerifyRaw) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsVerified;
                if (waited != word) ++CaptureTotals().wordMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word);
                word = waited;
            }
        } else {
            // On a word-wise page the bytes before the hook's wait are kept for the observer.
            const bool observed = page.wordwise && policy == PendingWrite::Sync && self.observe != nullptr && GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word));
            std::uint32_t before = 0;
            if (observed) std::memcpy(&before, reinterpret_cast<const void*>(address), sizeof(before));
            const auto waitsBefore = observed ? hookWaits() : 0;
            const auto started = page.wordwise ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::Read(address, std::as_writable_bytes(std::span(&word, 1)), alignof(std::uint32_t));
            if (page.wordwise) {
                const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                if (nanoseconds >= WordWaitNanoseconds) {
                    ++CaptureTotals().wordWaits;
                    CaptureTotals().wordWaitNanoseconds += nanoseconds;
                }
            }
            if (observed) report(waitsBefore, before == word);
        }
        if (CaptureProfiled()) CaptureTotals().wordReadNanoseconds += NanosecondsSince(fetchStarted);
        page.words[index] = word;
        page.valid.set(index);
    }
    page.read.set(index);
    page.recent.set(index);
    *value = page.words[index];
    return true;
}

bool ShaderMemory::accessible(void* context, std::uint64_t address, std::uint64_t bytes) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (bytes == 0 || address > std::numeric_limits<std::uint64_t>::max() - bytes) return false;
    const auto end = address + bytes;
    while (address < end) {
        const auto next = address < self.initialHigh ? self.initialAfter(address) : self.initial.begin();
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            if (address - previous->first < previous->second.size()) {
                address = previous->first + previous->second.size();
                continue;
            }
        }
        const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto pageEnd = std::min<std::uint64_t>(base + PageBytes, end);
        const auto& page = self.page(base);
        if (!page.wordwise && !page.whole && !page.valid.all() && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(pageEnd - address))) return false;
        address = pageEnd;
    }
    return true;
}

ShaderMemory::KnownValueCounts ShaderMemory::KnownValues() {
    auto& totals = CaptureTotals();
    return {totals.wordsKnown.load(std::memory_order_relaxed), totals.wordsKnownVerified.load(std::memory_order_relaxed), totals.wordsKnownMismatches.load(std::memory_order_relaxed)};
}

void ShaderMemory::CountHandleMemo(bool hit) {
    if (!CaptureProfiled()) return;
    (hit ? CaptureTotals().handleHits : CaptureTotals().handleMisses).fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle) {
    return capture(request, handle, nullptr);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::PreparedShaderInvocation& invocation) {
    return capture(invocation.Request(), nullptr, &invocation);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle, const ShaderRecompiler::PreparedShaderInvocation* invocation) {
    // The capture's word and page reads (through `read`) are attributed to it ([hooksync], [guestmem]).
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    const bool profile = CaptureProfiled();
    auto& totals = CaptureTotals();
    // The hook's GPU waits made anywhere inside the capture.
    const auto waitedBefore = profile ? WaitedMs() : 0.0;
    const auto started = std::chrono::steady_clock::now();
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = request.context.userData;
    runtime.shaderBase = request.shader.codeAddress;
    runtime.userContext = this;
    runtime.readMemory = &read;
    runtime.readSpecializationMemory = &read;
    runtime.accessible = &accessible;
    auto capture = invocation != nullptr ? invocation->Capture(runtime) : handle != nullptr ? ShaderRecompiler::CaptureResources(request, runtime, *handle) : ShaderRecompiler::CaptureResources(request, runtime);
    if (profile) {
        totals.captureNanoseconds += NanosecondsSince(started);
        totals.resolveNanoseconds += capture->sourceNanoseconds;
        totals.hookWaitNanoseconds += static_cast<std::uint64_t>((WaitedMs() - waitedBefore) * 1e6);
        std::uint64_t initialBytes = 0;
        for (const auto& [address, bytes] : initial) initialBytes += bytes.size();
        if (++totals.captures % 500 == 0) {
            const auto captures = static_cast<double>(totals.captures.load());
            const auto captureNs = totals.captureNanoseconds.load();
            const auto resolveNs = totals.resolveNanoseconds.load();
            const auto pageNs = totals.pageReadNanoseconds.load();
            const auto wordNs = totals.wordReadNanoseconds.load();
            const auto specializationNs = ShaderRecompiler::ResourceMaterializer::SpecializationNanoseconds();
            const auto hookNs = totals.hookWaitNanoseconds.load();
            const auto walkNs = captureNs > resolveNs + pageNs + wordNs + specializationNs ? captureNs - resolveNs - pageNs - wordNs - specializationNs : 0;
            AgcDriver::ProfilePrint_nid_no_patch( "[capture] %llu captures: %llu word reads, %llu pages fetched (%llu read word by word over pending GPU writes: %llu words waited %.2f s, %llu read raw on evidence, %llu verified with %llu mismatches, %llu served from known values, %llu hook reads not observed: no GPU wait), capture (plan lookup + materialize) %.1f s, %llu KiB registered code this capture; sub-phases (s, us per capture): source resolve %.2f/%.1f, walk %.2f/%.1f, specialization (every materialize) %.2f/%.1f, page reads %.2f/%.1f, word reads %.2f/%.1f, hook GPU waits inside %.2f/%.1f; source handle memo %llu hits, %llu resolves\n", static_cast<unsigned long long>(totals.captures.load()), static_cast<unsigned long long>(totals.reads.load()), static_cast<unsigned long long>(totals.pages.load()), static_cast<unsigned long long>(totals.pagesWordwise.load()), static_cast<unsigned long long>(totals.wordWaits.load()), totals.wordWaitNanoseconds.load() / 1e9, static_cast<unsigned long long>(totals.wordsRaw.load()), static_cast<unsigned long long>(totals.wordsVerified.load()), static_cast<unsigned long long>(totals.wordMismatches.load()), static_cast<unsigned long long>(totals.wordsKnown.load()), static_cast<unsigned long long>(totals.observationsSkipped.load()), captureNs / 1e9, static_cast<unsigned long long>(initialBytes / 1024), resolveNs / 1e9, resolveNs / captures / 1e3, walkNs / 1e9, walkNs / captures / 1e3, specializationNs / 1e9, specializationNs / captures / 1e3, pageNs / 1e9, pageNs / captures / 1e3, wordNs / 1e9, wordNs / captures / 1e3, hookNs / 1e9, hookNs / captures / 1e3, static_cast<unsigned long long>(totals.handleHits.load()), static_cast<unsigned long long>(totals.handleMisses.load()));
        }
    }
    return capture;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    Regions(result);
    return result;
}

void ShaderMemory::Regions(std::vector<ShaderRecompiler::MemoryRegion>& result) const {
    result.clear();
    result.reserve(initial.size() + pagesUsed);
    auto next = initial.begin();
    // Both are ordered by address and never overlap, so a merge keeps the result sorted.
    for (std::size_t used = 0; used < pagesUsed; ++used) {
        const auto base = pages[used].first;
        const auto& page = pages[used].second;
        while (next != initial.end() && next->first < base) {
            result.push_back({next->first, next->second});
            ++next;
        }
        forEachRun(page->read, [&](std::size_t first, std::size_t end) {
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page->words).subspan(first, end - first))});
        });
    }
    for (; next != initial.end(); ++next) result.push_back({next->first, next->second});
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::TakeRecentRegions() {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    TakeRecentRegions(result);
    return result;
}

void ShaderMemory::TakeRecentRegions(std::vector<ShaderRecompiler::MemoryRegion>& result) {
    result.clear();
    for (std::size_t used = 0; used < pagesUsed; ++used) {
        const auto base = pages[used].first;
        auto& page = pages[used].second;
        if (page->recent.none()) continue;
        forEachRun(page->recent, [&](std::size_t first, std::size_t end) {
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page->words).subspan(first, end - first))});
        });
        page->recent.reset();
    }
}

DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots) {
    DataWordPositionCounts counts;
    positions.clear();
    slots.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& [slot, address] : leaves) {
        if ((address & 3u) != 0) {
            ++counts.unmapped;
            continue;
        }
        if (std::binary_search(otherReads.begin(), otherReads.end(), address)) {
            ++counts.aliased;
            continue;
        }
        auto run = std::upper_bound(runs.begin(), runs.end(), address, [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& candidate) { return value < candidate.first; });
        if (run == runs.begin() || address + sizeof(std::uint32_t) > (run - 1)->second) {
            ++counts.unmapped;
            continue;
        }
        --run;
        const auto position = prefix[static_cast<std::size_t>(run - runs.begin())] + static_cast<std::size_t>((address - run->first) / sizeof(std::uint32_t));
        if (position >= words.size() || slot >= flattenedSrt.size() || words[position] != flattenedSrt[slot]) {
            ++counts.mismatched;
            continue;
        }
        found.emplace_back(static_cast<std::uint32_t>(position), slot);
    }
    std::sort(found.begin(), found.end());
    positions.reserve(found.size());
    slots.reserve(found.size());
    for (const auto& [position, slot] : found) {
        positions.push_back(position);
        slots.push_back(slot);
    }
    return counts;
}

}
