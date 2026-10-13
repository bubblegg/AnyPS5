#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

extern "C" char __ImageBase;

namespace AgcDriver::Graphics {
namespace {

const void* TraceModuleBase() {
    return &__ImageBase;
}

constexpr std::uint64_t KeyBytes = 256;

// Whether every key in [keys, keys + count) equals `first`. Eight keys per compare: the scan runs
// on every texture and storage validation, and an all-0xff (uncompressed) surface's keys are read
// to the end each time (132 KiB for a 4K RGBA8 target). APS5_NO_DCC_WORD_SCAN=1 restores the byte loop.
bool AllKeysEqual(const std::uint8_t* keys, std::size_t count, std::uint8_t first) {
    static const bool byteScan = std::getenv("APS5_NO_DCC_WORD_SCAN") != nullptr;
    if (byteScan) return std::all_of(keys, keys + count, [&](std::uint8_t key) { return key == first; });
    const std::uint64_t pattern = 0x0101010101010101ull * first;
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, keys + i, sizeof(word));
        if (word != pattern) return false;
    }
    for (; i < count; ++i) {
        if (keys[i] != first) return false;
    }
    return true;
}

DccKeys ByteKeys(std::uint8_t key) {
    switch (key) {
        case 0x00: return DccKeys::Clear0000;
        case 0x40: return DccKeys::Clear0001;
        case 0x80: return DccKeys::Clear1110;
        case 0xc0: return DccKeys::Clear1111;
        case 0x20: return DccKeys::ClearRegister;
        case 0x10: return DccKeys::ClearSingle;
        case 0xff: return DccKeys::Uncompressed;
        default: return DccKeys::Mixed;
    }
}

bool SingleKeysReadable(const GuestTextureResource& resource) {
    return KeyBytes % BytesPerElement(resource.format) == 0;
}

void ExpandSingleBlocks(std::span<std::byte> bytes, std::size_t elementBytes) {
    for (std::size_t block = 0; block < bytes.size(); block += KeyBytes) {
        const auto blockBytes = std::min<std::size_t>(KeyBytes, bytes.size() - block);
        auto* first = bytes.data() + block;
        for (auto filled = std::min(elementBytes, blockBytes); filled < blockBytes; filled *= 2) std::memcpy(first + filled, first, std::min(filled, blockBytes - filled));
    }
}

// APS5_PROFILE_DRAW: key scans, their bytes and time, printed as [dcc] every 10 s, with the hits of
// the GPU-store memo below and the "uncompressed" key stores by where they were made. Flush syncs
// stay 0 (no key read syncs the recorder); the field is kept so the line's shape does not change.
struct ScanProfile {
    std::atomic<std::uint64_t> scans{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::atomic<std::uint64_t> memoHits{0};
    std::atomic<std::uint64_t> flushSyncs{0};
    std::atomic<std::uint64_t> gpuStores{0};
    // GPU stores whose range had a 4-byte unaligned head or tail (copied instead of filled).
    std::atomic<std::uint64_t> gpuSplitStores{0};
    std::atomic<std::uint64_t> cpuStores{0};
    std::atomic<std::int64_t> lastReport{0};
};

ScanProfile& Scans() {
    static ScanProfile profile;
    return profile;
}

// ProvedClearKeys outcomes, counted with or without profiling (the tests read them).
struct ProofCounters {
    std::atomic<std::uint64_t> proved{0};
    std::atomic<std::uint64_t> scanned{0};
    std::atomic<std::uint64_t> unstable{0};
    std::atomic<std::uint64_t> rangeProved{0};
    std::atomic<std::uint64_t> rangeScanned{0};
};

ProofCounters& Proofs() {
    static ProofCounters counters;
    return counters;
}

bool ScanProfileEnabled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

void ReportScans(std::chrono::steady_clock::time_point now) {
    auto& profile = Scans();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto last = profile.lastReport.load();
    // The first event starts the period rather than reporting an empty one.
    if (last == 0) {
        profile.lastReport.compare_exchange_strong(last, nowMs);
        return;
    }
    if (nowMs - last < 10000 || !profile.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto& proofs = Proofs();
    std::fprintf(stderr, "[dcc] %llu scans, %.1f MiB scanned, %llu memo hits, %llu flush syncs, %.1f ms; uncompressed keys stored: %llu on the GPU (%llu with an unaligned head or tail), %llu on the CPU; key proofs: %llu proved, %llu scanned, %llu unstable\n", static_cast<unsigned long long>(profile.scans.load()), profile.bytes.load() / 1048576.0, static_cast<unsigned long long>(profile.memoHits.load()), static_cast<unsigned long long>(profile.flushSyncs.load()), profile.nanoseconds.load() / 1e6, static_cast<unsigned long long>(profile.gpuStores.load()), static_cast<unsigned long long>(profile.gpuSplitStores.load()), static_cast<unsigned long long>(profile.cpuStores.load()), static_cast<unsigned long long>(proofs.proved.load()), static_cast<unsigned long long>(proofs.scanned.load()), static_cast<unsigned long long>(proofs.unstable.load()));
}

void CountScan(std::size_t bytes, std::chrono::steady_clock::time_point start) {
    auto& profile = Scans();
    const auto now = std::chrono::steady_clock::now();
    profile.scans.fetch_add(1, std::memory_order_relaxed);
    profile.bytes.fetch_add(bytes, std::memory_order_relaxed);
    profile.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), std::memory_order_relaxed);
    ReportScans(now);
}

void CountStore(std::atomic<std::uint64_t>& counter) {
    if (!ScanProfileEnabled()) return;
    counter.fetch_add(1, std::memory_order_relaxed);
    ReportScans(std::chrono::steady_clock::now());
}

// "Uncompressed" keys the driver stored on the GPU (MarkDccUncompressed's recorded fill) whose batch
// may not have run yet: the bytes ReadDccKeys scans would still show the keys from before the store
// (a fast-clear code), and a storage image whose write-back made the store would be re-uploaded as
// cleared. While recorded work that writes the range is still running, the range reads as
// uncompressed from here; the bytes are trusted again only once the newest recorded writer of the
// range has signaled its fence (batches of the recorder's queue finish in order, so the fill and
// every writer up to that one have landed and the bytes are final, whichever batch wrote last).
// Dropping an entry on a later batch's serial alone would not do: until the fill's batch runs the
// bytes PREDATE the fill (the previous clear code), and a later writer that is not a same-code clear
// (a decompress kernel, a partial key write, a false overlap from an unrelated V#) would make the
// next Refresh compare that stale code against the uploaded keys. The lookup runs with or without
// GuestMemory::GpuMutex (a resource build's stage A reads keys unlocked, its stage B re-reads them
// under the lock): unlocked it can only ask the lock-free snapshot whether the range is written at
// all. Entries belong to one recorder: when the active recorder changes (device replacement; a new
// recorder can reuse the old one's address) they are all dropped. Its mutex is a leaf (nothing is
// taken under it).
struct GpuKeyStore {
    std::uint64_t begin;
    std::uint64_t end;
    DccKeys keys = DccKeys::Uncompressed;
    std::uint64_t note = 0;
};

struct KeyStoreMemo {
    std::mutex mutex;
    std::vector<GpuKeyStore> entries;
    const Recorder* recorder = nullptr;
};

KeyStoreMemo& Memo() {
    static KeyStoreMemo memo;
    return memo;
}

// Under memo.mutex: the active recorder, with the entries of any other recorder dropped.
Recorder* MemoRecorder(KeyStoreMemo& memo) {
    auto* active = Recorder::Active();
    if (active != memo.recorder) {
        memo.entries.clear();
        memo.recorder = active;
    }
    return active;
}

void ForgetStores(std::uint64_t begin, std::uint64_t end) {
    auto& memo = Memo();
    std::lock_guard lock(memo.mutex);
    std::erase_if(memo.entries, [&](const GpuKeyStore& store) { return begin < store.end && store.begin < end; });
}

std::optional<DccKeys> MemoizedKeys(std::uint64_t begin, std::uint64_t end) {
    auto& memo = Memo();
    std::lock_guard lock(memo.mutex);
    auto* recorder = MemoRecorder(memo);
    for (auto it = memo.entries.begin(); it != memo.entries.end(); ++it) {
        if (it->begin > begin || it->end < end) continue;
        const auto bytes = static_cast<std::size_t>(it->end - it->begin);
        bool pending = false;
        if (recorder != nullptr) {
            if (GuestMemory::GpuMutex().HeldByThisThread()) {
                // The open batch is never signaled (also with APS5_NO_SYNC_THROUGH, where the info
                // carries the open flag with the newest in-flight batch's fence state).
                const auto info = recorder->DescribePendingWrite(it->begin, bytes);
                pending = info.has_value() && !info->signaled;
            } else {
                pending = Recorder::SnapshotWriteOverlaps(it->begin, bytes);
            }
        }
        if (!pending) {
            memo.entries.erase(it);
            return std::nullopt;
        }
        if (ScanProfileEnabled()) Scans().memoHits.fetch_add(1, std::memory_order_relaxed);
        return it->keys;
    }
    return std::nullopt;
}

// Debug aid (APS5_TRACE_DCC_KEYS=1): every uncompressed key store, with the packet that made it.
void TraceKeyStore(const char* path, std::uint64_t begin, std::size_t count) {
    static const bool trace = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
    if (!trace) return;
    const auto packet = GuestMemory::CurrentPacket();
    std::fprintf(stderr, "[dcc-keys] uncompressed keys stored on the %s: 0x%llx+0x%zx (packet 0x%x queue 0x%x)\n", path, static_cast<unsigned long long>(begin), count, packet.opcode, packet.queue);
}

// The 0xff keys as a fill recorded into the active recorder's open batch, into the range's host
// import (the WriteLabelOnGpu / FillBuffer pattern): ordered behind every earlier recorded read or
// write of the range (the title's DCC clear or decompress kernel storing keys through a V#, which the
// CPU store had to wait for) and visible to the work recorded after it and to the host. vkCmdFillBuffer
// needs a 4-byte aligned offset and size; an unaligned head or tail is copied from a kept 4-byte
// buffer of 0xff instead. Under GuestMemory::GpuMutex (it records). False when the keys cannot be
// stored this way (memory the GPU has no view of): the caller stores them on the CPU.
bool StoreUncompressedOnGpu(const Context& context, Recorder& recorder, std::uint64_t begin, std::size_t count) {
    TraceGuestFill("driver DCC keys (GPU)", begin, count, 0xffffffffu);
    const auto* import = HostImportFor(context, begin, count);
    if (import == nullptr) return false;
    const VkDeviceSize first = begin - import->base;
    const VkDeviceSize last = first + count;
    const VkDeviceSize fillBegin = std::min((first + 3) & ~VkDeviceSize{3}, last);
    const VkDeviceSize fillEnd = std::max(last & ~VkDeviceSize{3}, fillBegin);
    // Everything that can throw (the seed's allocation, the batch's command buffer) comes before the
    // first recorded command: a fill left in the batch without its note and memo entry would be an
    // unordered write of the keys.
    std::shared_ptr<Buffer> seed;
    if (fillBegin > first || last > fillEnd) {
        seed = std::make_shared<Buffer>(context, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memset(seed->Bytes().data(), 0xff, seed->Bytes().size());
        CountStore(Scans().gpuSplitStores);
    }
    // A unit shadow's results in the key range's edge units reach the import before the fill
    // lands over part of them (the stamp below then makes those units stale).
    if (AnyShadowedOverlaps(begin, count)) PublishShadow(begin, count, PublishScope::PartialUnits, PublishReason::Keys);
    // Queued on the open batch and recorded with the batch's other key stores as one run (or before
    // a later command writing the keys; see Recorder::QueueKeyStore).
    recorder.QueueKeyStore(import->buffer, first, last, std::move(seed), begin, begin + count);
    recorder.NotePendingFill(begin, count, 0xff);
    GuestMemory::MarkWritten(begin, count);
    // After the note: an unlocked memo lookup drops an entry the snapshot does not cover yet. The
    // entry is kept only for the active recorder (the one the flush hook and the lookups consult).
    {
        auto& memo = Memo();
        std::lock_guard lock(memo.mutex);
        const auto end = begin + count;
        std::erase_if(memo.entries, [&](const GpuKeyStore& store) { return begin < store.end && store.begin < end; });
        if (MemoRecorder(memo) == &recorder) memo.entries.push_back({begin, end, DccKeys::Uncompressed, recorder.LastWriteNote(begin, count)});
    }
    CountStore(Scans().gpuStores);
    TraceKeyStore("gpu", begin, count);
    return true;
}

void StoreUncompressedOnCpu(std::uint64_t begin, std::size_t count) {
    TraceGuestFill("driver DCC keys (CPU)", begin, count, 0xffffffffu);
    TraceKeyStore("cpu", begin, count);
    // The write waits for recorded work over the range (the flush hook), so the bytes are current after.
    ForgetStores(begin, begin + count);
    const std::vector<std::byte> uncompressed(count, std::byte{0xff});
    GuestMemory::Write(begin, uncompressed);
    CountStore(Scans().cpuStores);
}

}

void NoteKeysFillOnGpu(std::uint64_t begin, std::size_t count, DccKeys keys) {
    static const bool disabled = std::getenv("APS5_NO_KEYS_FILL_MEMO") != nullptr;
    if (disabled || count == 0) return;
    auto& memo = Memo();
    std::lock_guard lock(memo.mutex);
    const auto end = begin + count;
    std::erase_if(memo.entries, [&](const GpuKeyStore& store) { return begin < store.end && store.begin < end; });
    auto* recorder = MemoRecorder(memo);
    if (recorder != nullptr) memo.entries.push_back({begin, end, keys, GuestMemory::GpuMutex().HeldByThisThread() ? recorder->LastWriteNote(begin, count) : 0});
}

namespace {

std::optional<DccKeys> PendingStoreKeys(Recorder& recorder, std::uint64_t begin, std::size_t count) {
    const auto end = begin + count;
    std::uint64_t note = 0;
    DccKeys keys = DccKeys::Mixed;
    {
        auto& memo = Memo();
        std::lock_guard lock(memo.mutex);
        if (MemoRecorder(memo) != &recorder) return std::nullopt;
        for (const auto& store : memo.entries) {
            if (store.note == 0 || store.begin > begin || store.end < end) continue;
            note = store.note;
            keys = store.keys;
            break;
        }
    }
    if (note == 0 || AnyShadowedOverlaps(begin, count) || recorder.NewestWriteNote(begin, count) != note) return std::nullopt;
    return keys;
}

// Whether the keys already read as uncompressed: scans the same bytes as ReadDccKeys, so it counts
// as a scan too.
bool AlreadyUncompressed(std::uint64_t metaAddress, std::size_t count) {
    const auto* keys = reinterpret_cast<const std::uint8_t*>(metaAddress);
    const auto start = ScanProfileEnabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool uncompressed = AllKeysEqual(keys, count, 0xff);
    if (ScanProfileEnabled()) CountScan(count, start);
    return uncompressed;
}

// Whether the bytes at a key range still read as DCC keys. A title frees a target's metadata before
// the driver has finished with the target; its allocator then keeps list pointers in the first bytes
// of the block or hands it to something else, and keys stored there corrupt the title's heap. Every
// key byte is one of six codes, so the head of the range and a sample of the rest are checked.
bool LooksLikeKeys(std::uint64_t metaAddress, std::size_t count) {
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(metaAddress), count)) return false;
    const auto* keys = reinterpret_cast<const std::uint8_t*>(metaAddress);
    const auto code = [](std::uint8_t key) { return key == 0x00 || key == 0x20 || key == 0x40 || key == 0x80 || key == 0xc0 || key == 0xff; };
    const auto head = std::min<std::size_t>(count, 64);
    for (std::size_t i = 0; i < head; ++i) {
        if (!code(keys[i])) return false;
    }
    for (std::size_t i = head; i < count; i += 61) {
        if (!code(keys[i])) return false;
    }
    return true;
}

bool KeysStillOwned(std::uint64_t metaAddress, std::size_t count) {
    static const bool unchecked = std::getenv("APS5_UNCHECKED_DCC_STORES") != nullptr;
    if (unchecked || LooksLikeKeys(metaAddress, count)) return true;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 12) std::fprintf(stderr, "[gpu] DCC metadata 0x%llx (0x%zx keys) no longer holds keys: the title reused the memory, so nothing is stored there\n", static_cast<unsigned long long>(metaAddress), count);
    return false;
}

bool CpuKeysOnly() {
    static const bool cpuKeys = std::getenv("APS5_CPU_DCC_KEYS") != nullptr;
    return cpuKeys;
}

enum class Kind { Unorm, Snorm, Uint, Sint, Float };

struct Layout {
    std::uint32_t channels;
    std::uint32_t bits[4];
    Kind kind;
};

bool LayoutFor(VkFormat format, Layout& layout) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: layout = {1, {8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8_UINT: layout = {1, {8}, Kind::Uint}; return true;
        case VK_FORMAT_R16_UNORM: layout = {1, {16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16_SNORM: layout = {1, {16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16_UINT: layout = {1, {16}, Kind::Uint}; return true;
        case VK_FORMAT_R16_SINT: layout = {1, {16}, Kind::Sint}; return true;
        case VK_FORMAT_R16_SFLOAT: layout = {1, {16}, Kind::Float}; return true;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB: layout = {2, {8, 8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8G8_SNORM: layout = {2, {8, 8}, Kind::Snorm}; return true;
        case VK_FORMAT_R8G8_UINT: layout = {2, {8, 8}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8_SINT: layout = {2, {8, 8}, Kind::Sint}; return true;
        case VK_FORMAT_R32_UINT: layout = {1, {32}, Kind::Uint}; return true;
        case VK_FORMAT_R32_SINT: layout = {1, {32}, Kind::Sint}; return true;
        case VK_FORMAT_R32_SFLOAT: layout = {1, {32}, Kind::Float}; return true;
        case VK_FORMAT_R16G16_UNORM: layout = {2, {16, 16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16G16_SNORM: layout = {2, {16, 16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16G16_UINT: layout = {2, {16, 16}, Kind::Uint}; return true;
        case VK_FORMAT_R16G16_SINT: layout = {2, {16, 16}, Kind::Sint}; return true;
        case VK_FORMAT_R16G16_SFLOAT: layout = {2, {16, 16}, Kind::Float}; return true;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: layout = {3, {11, 11, 10}, Kind::Float}; return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: layout = {4, {10, 10, 10, 2}, Kind::Unorm}; return true;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: layout = {4, {10, 10, 10, 2}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: layout = {4, {8, 8, 8, 8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8G8B8A8_SNORM: layout = {4, {8, 8, 8, 8}, Kind::Snorm}; return true;
        case VK_FORMAT_R8G8B8A8_UINT: layout = {4, {8, 8, 8, 8}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8B8A8_SINT: layout = {4, {8, 8, 8, 8}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32_UINT: layout = {2, {32, 32}, Kind::Uint}; return true;
        case VK_FORMAT_R32G32_SINT: layout = {2, {32, 32}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32_SFLOAT: layout = {2, {32, 32}, Kind::Float}; return true;
        case VK_FORMAT_R16G16B16A16_UNORM: layout = {4, {16, 16, 16, 16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16G16B16A16_SNORM: layout = {4, {16, 16, 16, 16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16G16B16A16_UINT: layout = {4, {16, 16, 16, 16}, Kind::Uint}; return true;
        case VK_FORMAT_R16G16B16A16_SINT: layout = {4, {16, 16, 16, 16}, Kind::Sint}; return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT: layout = {4, {16, 16, 16, 16}, Kind::Float}; return true;
        case VK_FORMAT_R32G32B32A32_UINT: layout = {4, {32, 32, 32, 32}, Kind::Uint}; return true;
        case VK_FORMAT_R32G32B32A32_SINT: layout = {4, {32, 32, 32, 32}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT: layout = {4, {32, 32, 32, 32}, Kind::Float}; return true;
        default: return false;
    }
}

// "1" in a channel of this kind and width.
std::uint64_t One(Kind kind, std::uint32_t bits) {
    switch (kind) {
        case Kind::Unorm: case Kind::Uint: return bits >= 64 ? ~0ull : (1ull << bits) - 1u;
        case Kind::Snorm: case Kind::Sint: return (1ull << (bits - 1u)) - 1u;
        case Kind::Float:
            switch (bits) {
                case 10: return 0x1e0u;
                case 11: return 0x3c0u;
                case 16: return 0x3c00u;
                case 32: return 0x3f800000u;
                default: throw std::runtime_error("AGC graphics: DCC clear of a " + std::to_string(bits) + "-bit float channel is not implemented");
            }
    }
    return 0;
}

}

const char* DccKeysName(DccKeys keys) {
    switch (keys) {
        case DccKeys::Uncompressed: return "uncompressed";
        case DccKeys::Clear0000: return "0000";
        case DccKeys::Clear0001: return "0001";
        case DccKeys::Clear1110: return "1110";
        case DccKeys::Clear1111: return "1111";
        case DccKeys::ClearRegister: return "register";
        case DccKeys::ClearSingle: return "single";
        case DccKeys::Mixed: return "mixed";
        case DccKeys::Unreadable: return "unreadable";
    }
    return "?";
}

std::size_t DccKeyBytes(std::uint64_t surfaceBytes) {
    return static_cast<std::size_t>(surfaceBytes / KeyBytes);
}

std::size_t DccKeyCount(TextureTileMode tileMode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint64_t surfaceBytes) {
    const auto keys = DccKeyBytes(surfaceBytes);
    if (tileMode != TextureTileMode::kR64KBX || elementBytes != 4) return keys;
    constexpr std::uint64_t MetaBlockEdge = 512;
    constexpr std::uint64_t MetaBlockBytes = 4096;
    const auto console = (width + MetaBlockEdge - 1) / MetaBlockEdge * ((height + MetaBlockEdge - 1) / MetaBlockEdge) * MetaBlockBytes;
    return std::max(keys, static_cast<std::size_t>(console));
}

std::size_t DccKeyCount(const GuestTextureResource& surface, std::uint64_t surfaceBytes) {
    const bool single = surface.mipCount == 1 && surface.baseArray == 0 && surface.depthOrLastArray == 0 && (surface.dimension == TextureDimension::k2D || surface.dimension == TextureDimension::k2DArray);
    if (!single || !surface.dccPipeAligned || surface.tileMode != TextureTileMode::kR64KBX) return DccKeyBytes(surfaceBytes);
    return DccKeyCount(surface.tileMode, BytesPerElement(surface.format), surface.width, surface.height, surfaceBytes);
}

namespace {

// ReadDccKeys, saying in `memoized` whether the answer came from the pending-store memo rather
// than the bytes.
DccKeys readDccKeys(std::uint64_t metaAddress, std::size_t count, bool& memoized) {
    memoized = false;
    if (metaAddress == 0 || count == 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(metaAddress), count)) return DccKeys::Unreadable;
    // A driver store still pending on the GPU is not in the bytes yet.
    if (const auto memo = MemoizedKeys(metaAddress, metaAddress + count)) {
        memoized = true;
        return *memo;
    }
    // Keys under a unit shadow's fresh results (metadata aliased with a surface): under the
    // device lock they are published and waited for; a build's unlocked stage answers "unstable"
    // so its stage B reads them under the lock.
    if (AnyShadowedOverlaps(metaAddress, count)) {
        if (!GuestMemory::GpuMutex().HeldByThisThread()) {
            memoized = true;
            return DccKeys::Mixed;
        }
        PublishShadow(metaAddress, count, PublishScope::Whole, PublishReason::Keys);
        if (auto* recorder = Recorder::Active()) {
            Recorder::CountSync(2);
            recorder->Sync();
        }
    }
    const auto* keys = reinterpret_cast<const std::uint8_t*>(metaAddress);
    const auto first = keys[0];
    const auto start = ScanProfileEnabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool uniform = AllKeysEqual(keys + 1, count - 1, first);
    if (ScanProfileEnabled()) CountScan(count, start);
    return uniform ? ByteKeys(first) : DccKeys::Mixed;
}

DccKeys textureClearKeys(const GuestTextureResource& resource, std::uint64_t guestBytes, bool& memoized) {
    memoized = false;
    if (resource.dccAddress == 0) return DccKeys::Uncompressed;
    DccKeys keys;
    if (const auto known = WaitForKeyWriters(resource, guestBytes)) keys = *known;
    else keys = readDccKeys(resource.dccAddress, DccKeyBytes(guestBytes), memoized);
    if (keys == DccKeys::Uncompressed || (keys == DccKeys::ClearSingle && SingleKeysReadable(resource))) return keys;
    if (IsConvertedTextureFormat(resource.format) && (keys == DccKeys::Clear0001 || keys == DccKeys::Clear1110)) throw std::runtime_error(std::string("AGC graphics: DCC clear code ") + DccKeysName(keys) + " of converted texture format " + std::to_string(resource.format) + " is not implemented");
    std::byte probe[16]{};
    if (!IsDccClear(keys) || !FillDccClear(ResolveTextureFormat(resource.format), keys, resource.dccAlphaOnMsb, std::span(probe, std::min<std::size_t>(sizeof(probe), BytesPerElement(resource.format))))) {
        static std::mutex reportedMutex;
        static std::set<std::pair<std::uint64_t, int>> reported;
        std::lock_guard lock(reportedMutex);
        if (reported.size() < 32 && reported.insert({resource.baseAddress, static_cast<int>(keys)}).second) std::fprintf(stderr, "[gpu] texture 0x%llx (format %u) has %s DCC keys at 0x%llx; its texels are read as stored\n", static_cast<unsigned long long>(resource.baseAddress), resource.format, DccKeysName(keys), static_cast<unsigned long long>(resource.dccAddress));
        return DccKeys::Uncompressed;
    }
    return keys;
}

DccKeys currentDccKeys(std::uint64_t metaAddress, std::size_t count, bool& memoized) {
    memoized = false;
    if (metaAddress != 0 && count != 0 && GuestMemory::GpuMutex().HeldByThisThread()) {
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->PendingWriteOverlaps(metaAddress, count)) {
            if (const auto keys = PendingStoreKeys(*recorder, metaAddress, count)) {
                memoized = true;
                return *keys;
            }
            Recorder::CountSync(2);
            recorder->SyncThrough(metaAddress, count);
        }
    }
    return readDccKeys(metaAddress, count, memoized);
}

// The longest fill a title made from each address (NoteTitleMetadataFill): how much metadata it owns there.
std::mutex& titleFillsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::uint64_t, std::size_t>& titleFills() {
    static auto* fills = new std::map<std::uint64_t, std::size_t>();
    return *fills;
}

// A key count above one per 256 surface bytes is the console's rounded-up extent (DccKeyCount). A
// title may allocate less than that from its own heap (a 1920x1080 target: about 48000 bytes against
// 49152), and keys stored past its allocation land in the next heap block. The extra keys are used
// only as far as the title itself has filled from this address, and not at all when it never did.
std::size_t titleKeyCount(std::uint64_t metaAddress, std::size_t keys, std::size_t keyCount) {
    if (keyCount <= keys) return keyCount;
    std::size_t filled = 0;
    {
        std::lock_guard lock(titleFillsMutex());
        const auto found = titleFills().find(metaAddress);
        if (found != titleFills().end()) filled = found->second;
    }
    const auto count = std::min(keyCount, std::max(filled, keys));
    if (count != keyCount) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[gpu] DCC metadata 0x%llx: 0x%zx keys are used, not the console extent 0x%zx (the title filled 0x%zx from there; one key per 256 surface bytes is 0x%zx)\n", static_cast<unsigned long long>(metaAddress), count, keyCount, filled, keys);
    }
    return count;
}

std::size_t StoredKeyCount(std::uint64_t metaAddress, std::uint64_t surfaceBytes, std::size_t keyCount) {
    const auto keys = DccKeyBytes(surfaceBytes);
    Require(keyCount >= keys, "a DCC key range covers fewer keys than one per 256 surface bytes");
    keyCount = titleKeyCount(metaAddress, keys, keyCount);
    if (metaAddress == 0 || keyCount == 0) return 0;
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(metaAddress), keyCount, true)) return keyCount;
    if (keyCount == keys) return 0;
    char message[256];
    std::snprintf(message, sizeof(message), "AGC graphics: DCC metadata 0x%llx is not writable over the 0x%zx key bytes of its surface (0x%zx at one per 256 bytes): metadata smaller than the console's DCC extent is not modeled", static_cast<unsigned long long>(metaAddress), keyCount, keys);
    throw std::runtime_error(message);
}

}

DccKeys ReadDccKeys(std::uint64_t metaAddress, std::uint64_t surfaceBytes) {
    bool memoized = false;
    return readDccKeys(metaAddress, DccKeyBytes(surfaceBytes), memoized);
}

DccKeys CurrentDccKeys(std::uint64_t metaAddress, std::uint64_t surfaceBytes) {
    bool memoized = false;
    return currentDccKeys(metaAddress, DccKeyBytes(surfaceBytes), memoized);
}

DccKeys CurrentDccKeys(std::uint64_t metaAddress, std::uint64_t surfaceBytes, std::size_t keyCount) {
    Require(keyCount >= DccKeyBytes(surfaceBytes), "a DCC key range covers fewer keys than one per 256 surface bytes");
    bool memoized = false;
    return currentDccKeys(metaAddress, titleKeyCount(metaAddress, DccKeyBytes(surfaceBytes), keyCount), memoized);
}

void NoteTitleMetadataFill(std::uint64_t base, std::size_t bytes) {
    constexpr std::size_t LargestMetadata = 4u << 20u;
    if (bytes == 0 || bytes > LargestMetadata) return;
    std::lock_guard lock(titleFillsMutex());
    auto& fills = titleFills();
    if (fills.size() >= 16384 && !fills.contains(base)) return;
    auto& longest = fills[base];
    longest = std::max(longest, bytes);
}

void TraceGuestFill(const char* who, std::uint64_t base, std::size_t bytes, std::uint32_t word) {
    static const bool enabled = std::getenv("APS5_TRACE_GUEST_FILLS") != nullptr;
    if (!enabled) return;
    static std::mutex seenMutex;
    static std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    std::lock_guard lock(seenMutex);
    if (seen.size() >= 6000 || !seen.insert({base, (static_cast<std::uint64_t>(bytes) << 32u) ^ word}).second) return;
    std::fprintf(stderr, "[fill-trace] %s 0x%llx..0x%llx (0x%zx bytes) word 0x%08x\n", who, static_cast<unsigned long long>(base), static_cast<unsigned long long>(base + bytes), bytes, word);
}

bool IsDccClear(DccKeys keys) {
    return keys == DccKeys::Clear0000 || keys == DccKeys::Clear0001 || keys == DccKeys::Clear1110 || keys == DccKeys::Clear1111 || keys == DccKeys::ClearRegister;
}

void MarkDccUncompressed(std::uint64_t metaAddress, std::uint64_t surfaceBytes) {
    MarkDccUncompressed(metaAddress, surfaceBytes, DccKeyBytes(surfaceBytes));
}

void MarkDccUncompressed(std::uint64_t metaAddress, std::uint64_t surfaceBytes, std::size_t keyCount) {
    const auto count = StoredKeyCount(metaAddress, surfaceBytes, keyCount);
    if (count == 0) return;
    TraceGuestFill("driver marks DCC keys, caller", metaAddress, count, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - reinterpret_cast<std::uintptr_t>(TraceModuleBase())));
    if (!KeysStillOwned(metaAddress, count)) return;
    // The bytes are trusted only when no recorded work writes them (lock-free, with or without
    // GuestMemory::GpuMutex); otherwise the store runs and waits through the flush hook. A pending
    // GPU store of the driver's own is no reason to skip: a title kernel recorded into the same
    // batch after it may write clear keys, and the memo cannot order inside a batch.
    if (!Recorder::SnapshotWriteOverlaps(metaAddress, count) && AlreadyUncompressed(metaAddress, count)) return;
    StoreUncompressedOnCpu(metaAddress, count);
}

void MarkDccUncompressed(const Context& context, std::uint64_t metaAddress, std::uint64_t surfaceBytes) {
    MarkDccUncompressed(context, metaAddress, surfaceBytes, DccKeyBytes(surfaceBytes));
}

void MarkDccUncompressed(const Context& context, std::uint64_t metaAddress, std::uint64_t surfaceBytes, std::size_t keyCount) {
    const auto count = StoredKeyCount(metaAddress, surfaceBytes, keyCount);
    if (count == 0) return;
    TraceGuestFill("driver marks DCC keys, caller", metaAddress, count, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - reinterpret_cast<std::uintptr_t>(TraceModuleBase())));
    if (!KeysStillOwned(metaAddress, count)) return;
    auto* recorder = CpuKeysOnly() || !GuestMemory::GpuMutex().HeldByThisThread() ? nullptr : Recorder::Active();
    // The bytes are scanned only when no recorded work writes them: a pending clear kernel's keys are
    // not in memory yet, so the bytes could read as uncompressed while the kernel will store a clear
    // code, and the range is stored without looking (the fill lands after the kernel). That includes
    // a pending GPU store of the driver's own (the memo): a title kernel recorded into the same batch
    // after it may write clear keys, and nothing orders inside a batch, so a fresh fill is recorded
    // (idempotent, no wait) rather than trusting the earlier one.
    const bool pending = recorder != nullptr && recorder->PendingWriteOverlaps(metaAddress, count);
    if (!pending && AlreadyUncompressed(metaAddress, count)) return;
    if (recorder != nullptr && StoreUncompressedOnGpu(context, *recorder, metaAddress, count)) return;
    StoreUncompressedOnCpu(metaAddress, count);
}

bool FillDccClear(VkFormat format, DccKeys keys, bool alphaOnMsb, std::span<std::byte> bytes) {
    // The clear-register code names a color only the target's registers hold. A reader that has none
    // (a texture sampled before it is next bound as a target) gets zeros rather than whatever the
    // surface memory holds, which for an HDR target shows as blown-out blocks.
    if (keys == DccKeys::Clear0000 || keys == DccKeys::ClearRegister) {
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        return true;
    }
    Layout layout{};
    if (!IsDccClear(keys) || !LayoutFor(format, layout)) return false;
    const bool color = keys == DccKeys::Clear1110 || keys == DccKeys::Clear1111;
    const bool alpha = keys == DccKeys::Clear0001 || keys == DccKeys::Clear1111;
    const int alphaChannel = layout.channels == 3 ? -1 : alphaOnMsb ? static_cast<int>(layout.channels) - 1 : 0;
    std::uint32_t elementBits = 0;
    for (std::uint32_t channel = 0; channel < layout.channels; ++channel) elementBits += layout.bits[channel];
    std::vector<std::byte> element(elementBits / 8u);
    std::uint32_t bit = 0;
    for (std::uint32_t channel = 0; channel < layout.channels; ++channel) {
        const bool set = static_cast<int>(channel) == alphaChannel ? alpha : color;
        const auto value = set ? One(layout.kind, layout.bits[channel]) : 0u;
        for (std::uint32_t i = 0; i < layout.bits[channel]; ++i, ++bit) {
            if (((value >> i) & 1u) != 0) element[bit / 8u] |= static_cast<std::byte>(1u << (bit % 8u));
        }
    }
    for (std::size_t offset = 0; offset + element.size() <= bytes.size(); offset += element.size()) std::memcpy(bytes.data() + offset, element.data(), element.size());
    return true;
}

bool DccAlphaOnMsb(VkFormat format, std::uint32_t componentSwap) {
    Layout layout{};
    const auto channels = LayoutFor(format, layout) ? layout.channels : 4u;
    constexpr std::uint32_t standardReversed = 2;
    constexpr std::uint32_t alternateReversed = 3;
    if (channels == 1) return componentSwap == alternateReversed;
    return componentSwap != standardReversed && componentSwap != alternateReversed;
}

DccKeys TextureClearKeys(const GuestTextureResource& resource, std::uint64_t guestBytes) {
    bool memoized = false;
    return textureClearKeys(resource, guestBytes, memoized);
}

bool KeyFastPath() {
    static const bool enabled = std::getenv("APS5_NO_KEY_FAST_PATH") == nullptr;
    return enabled;
}

DccKeys ProvedClearKeys(const GuestTextureResource& resource, std::uint64_t guestBytes, DccKeyProof& proof) {
    if (resource.dccAddress == 0) return DccKeys::Uncompressed;
    auto& counters = Proofs();
    if (!KeyFastPath()) {
        counters.scanned.fetch_add(1, std::memory_order_relaxed);
        return TextureClearKeys(resource, guestBytes);
    }
    const auto count = static_cast<std::size_t>(guestBytes / KeyBytes);
    // The collect precedes the scan: a write landing between them is stamped above `collected`, so
    // the proof it would invalidate is never taken as current.
    const auto collected = count == 0 ? 0 : GuestMemory::CollectWrites(resource.dccAddress, count);
    if (proof.generation != 0 && collected != 0 && GuestMemory::UnchangedSince(resource.dccAddress, count, proof.generation)) {
        counters.proved.fetch_add(1, std::memory_order_relaxed);
        return proof.keys;
    }
    bool memoized = false;
    const auto keys = textureClearKeys(resource, guestBytes, memoized);
    bool stable = collected != 0 && !memoized;
    if (stable) {
        // After the scan: a write noted since the collect is stamped above it anyway; one noted
        // before it may still land after the bytes were read.
        if (GuestMemory::GpuMutex().HeldByThisThread()) {
            if (auto* recorder = Recorder::Active(); recorder != nullptr) {
                const auto info = recorder->DescribePendingWrite(resource.dccAddress, count);
                stable = !info.has_value() || info->signaled;
            }
        } else {
            stable = !Recorder::SnapshotWriteOverlaps(resource.dccAddress, count);
        }
    }
    proof = stable ? DccKeyProof{keys, collected} : DccKeyProof{};
    counters.scanned.fetch_add(1, std::memory_order_relaxed);
    if (!stable) counters.unstable.fetch_add(1, std::memory_order_relaxed);
    return keys;
}

DccKeyProofCounts KeyProofCounts() {
    const auto& counters = Proofs();
    return {counters.proved.load(std::memory_order_relaxed), counters.scanned.load(std::memory_order_relaxed), counters.unstable.load(std::memory_order_relaxed), counters.rangeProved.load(std::memory_order_relaxed), counters.rangeScanned.load(std::memory_order_relaxed)};
}

DccKeys ProvedCurrentDccKeys(std::uint64_t metaAddress, std::uint64_t surfaceBytes, DccRangeProof& proof) {
    const auto count = DccKeyBytes(surfaceBytes);
    if (metaAddress == 0 || count == 0 || !KeyFastPath() || !GuestMemory::GpuMutex().HeldByThisThread()) return CurrentDccKeys(metaAddress, surfaceBytes);
    auto& counters = Proofs();
    const auto collected = GuestMemory::CollectWrites(metaAddress, count);
    if (proof.generation != 0 && proof.address == metaAddress && proof.count == count && collected != 0 && GuestMemory::UnchangedSince(metaAddress, count, proof.generation)) {
        counters.rangeProved.fetch_add(1, std::memory_order_relaxed);
        return proof.keys;
    }
    bool memoized = false;
    const auto keys = currentDccKeys(metaAddress, count, memoized);
    bool stable = collected != 0 && !memoized;
    if (stable) {
        if (auto* recorder = Recorder::Active(); recorder != nullptr) {
            const auto info = recorder->DescribePendingWrite(metaAddress, count);
            stable = !info.has_value() || info->signaled;
        }
    }
    proof = stable ? DccRangeProof{metaAddress, count, keys, collected} : DccRangeProof{};
    counters.rangeScanned.fetch_add(1, std::memory_order_relaxed);
    return keys;
}

void ReadTextureSurface(const GuestTextureResource& resource, DccKeys keys, std::span<std::byte> bytes) {
    // Named for the [hooksync] attribution: the read goes through the flush hook.
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureRead);
    if (keys != DccKeys::Uncompressed && keys != DccKeys::ClearSingle) {
        FillDccClear(ResolveTextureFormat(resource.format), keys, resource.dccAlphaOnMsb, bytes);
        return;
    }
    GuestMemory::ReadCommitted(resource.baseAddress, bytes);
    if (keys == DccKeys::ClearSingle) {
        ExpandSingleBlocks(bytes, BytesPerElement(resource.format));
        return;
    }
    // Mixed keys: part of the surface is fast-cleared and part holds texels. The cleared parts hold
    // whatever was in memory (an HDR target shows it as blocks of noise), so each 256 bytes whose key
    // is a clear code get that clear color, each whose key is comp-to-single gets its first element
    // repeated, and the rest stay as stored. The keys are taken in surface-memory order, one per 256
    // bytes. APS5_NO_MIXED_DCC_CLEAR=1 reads everything as stored.
    static const bool mixedClear = std::getenv("APS5_NO_MIXED_DCC_CLEAR") == nullptr;
    const auto count = bytes.size() / KeyBytes;
    if (!mixedClear || resource.dccAddress == 0 || count == 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(resource.dccAddress), count)) return;
    const auto* stored = reinterpret_cast<const std::uint8_t*>(resource.dccAddress);
    if (AllKeysEqual(stored, count, 0xffu)) return;
    const auto format = ResolveTextureFormat(resource.format);
    const bool singleReadable = SingleKeysReadable(resource);
    for (std::size_t key = 0; key < count; ++key) {
        const auto code = ByteKeys(stored[key]);
        if (code == DccKeys::ClearSingle && singleReadable) ExpandSingleBlocks(bytes.subspan(key * KeyBytes, KeyBytes), BytesPerElement(resource.format));
        if (!IsDccClear(code)) continue;
        FillDccClear(format, code, resource.dccAlphaOnMsb, bytes.subspan(key * KeyBytes, KeyBytes));
    }
}

namespace {

DccKeys EncodedKeys(const GuestTextureResource& resource, DccKeys keys) {
    if (keys == DccKeys::ClearSingle && SingleKeysReadable(resource)) return keys;
    std::byte probe[16]{};
    if (!IsDccClear(keys) || !FillDccClear(ResolveTextureFormat(resource.format), keys, resource.dccAlphaOnMsb, std::span(probe, std::min<std::size_t>(sizeof(probe), BytesPerElement(resource.format))))) return DccKeys::Uncompressed;
    return keys;
}

bool PendingFillsLeaveUncompressed(const Recorder& recorder, const GuestTextureResource& resource, std::size_t count) {
    const auto begin = resource.dccAddress;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), count) || AnyShadowedOverlaps(begin, count)) return false;
    auto writes = recorder.PendingWritesOver(begin, count);
    std::sort(writes.begin(), writes.end(), [](const Recorder::PendingWrite& a, const Recorder::PendingWrite& b) { return a.note > b.note; });
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unwritten{{begin, begin + count}};
    int value = -1;
    bool mixed = false;
    const auto see = [&](int key) {
        if (value < 0) value = key;
        else if (value != key) mixed = true;
    };
    for (const auto& write : writes) {
        if (mixed || unwritten.empty()) break;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> rest;
        bool covered = false;
        for (const auto& [from, to] : unwritten) {
            if (write.end <= from || to <= write.begin) {
                rest.emplace_back(from, to);
                continue;
            }
            covered = true;
            if (from < write.begin) rest.emplace_back(from, write.begin);
            if (write.end < to) rest.emplace_back(write.end, to);
        }
        unwritten = std::move(rest);
        if (covered && write.value >= 0) see(write.value);
    }
    for (const auto& [from, to] : unwritten) {
        if (mixed) break;
        const auto* keys = reinterpret_cast<const std::uint8_t*>(from);
        if (AllKeysEqual(keys + 1, static_cast<std::size_t>(to - from) - 1, keys[0])) see(keys[0]);
        else mixed = true;
    }
    return mixed || (value >= 0 && EncodedKeys(resource, ByteKeys(static_cast<std::uint8_t>(value))) == DccKeys::Uncompressed);
}

// Measuring aid: the GPU waits WaitForKeyWriters makes, and the pending writes over the keys that
// caused them (by size and how they lie over the keys), printed every 10 s as [key-writer-sync].
// The key addresses whose pending writes made WaitForKeyWriters wait, for ReportKeyWriterDispatch.
struct WaitedKeys {
    std::mutex mutex;
    std::set<std::uint64_t> addresses;
};

WaitedKeys& waitedKeys() {
    static WaitedKeys keys;
    return keys;
}

void noteKeyWriterSync(const Recorder& recorder, std::uint64_t keys, std::size_t count) {
    {
        auto& waited = waitedKeys();
        std::lock_guard lock(waited.mutex);
        if (waited.addresses.size() < 64) waited.addresses.insert(keys);
    }
    struct Totals {
        std::uint64_t syncs = 0, writes = 0, exact = 0, covering = 0, inside = 0, partial = 0, filled = 0;
        std::array<std::uint64_t, 6> sizes{};
        std::uint64_t printed = 0;
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    };
    static Totals totals;
    ++totals.syncs;
    const auto end = keys + count;
    for (const auto& write : recorder.PendingWritesOver(keys, count)) {
        const auto size = write.end - write.begin;
        ++totals.writes;
        if (write.begin == keys && write.end == end) ++totals.exact;
        else if (write.begin <= keys && end <= write.end) ++totals.covering;
        else if (keys <= write.begin && write.end <= end) ++totals.inside;
        else ++totals.partial;
        if (write.value >= 0) ++totals.filled;
        ++totals.sizes[size <= 4096 ? 0 : size <= 65536 ? 1 : size <= 1048576 ? 2 : size <= 16777216 ? 3 : size <= 268435456 ? 4 : 5];
        if (totals.printed < 12) {
            ++totals.printed;
            std::fprintf(stderr, "[key-writer-sync] wait for keys 0x%llx+0x%zx: pending write 0x%llx+0x%llx (value %d, note %llu)\n", static_cast<unsigned long long>(keys), count, static_cast<unsigned long long>(write.begin), static_cast<unsigned long long>(size), write.value, static_cast<unsigned long long>(write.note));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.since < std::chrono::seconds(10)) return;
    std::fprintf(stderr, "[key-writer-sync] %.1f s: %llu GPU waits for DCC keys; pending writes over the keys %llu: exactly the keys %llu, covering them %llu, inside them %llu, partly %llu, fills %llu; by size <=4K %llu, <=64K %llu, <=1M %llu, <=16M %llu, <=256M %llu, larger %llu\n", std::chrono::duration<double>(now - totals.since).count(), static_cast<unsigned long long>(totals.syncs), static_cast<unsigned long long>(totals.writes), static_cast<unsigned long long>(totals.exact), static_cast<unsigned long long>(totals.covering), static_cast<unsigned long long>(totals.inside), static_cast<unsigned long long>(totals.partial), static_cast<unsigned long long>(totals.filled), static_cast<unsigned long long>(totals.sizes[0]), static_cast<unsigned long long>(totals.sizes[1]), static_cast<unsigned long long>(totals.sizes[2]), static_cast<unsigned long long>(totals.sizes[3]), static_cast<unsigned long long>(totals.sizes[4]), static_cast<unsigned long long>(totals.sizes[5]));
    const auto printed = totals.printed;
    totals = {};
    totals.printed = printed;
    totals.since = now;
}

// What a recognised kernel of the title is expected to leave in the DCC keys it is given: one byte
// throughout (NoteKeyKernelDispatch). Measured against the keys after each wait before anything
// relies on it.
struct KeyPrediction {
    std::uint64_t bytes = 0;
    int value = -1;
    char kernel = '?';
    std::uint32_t input = 0;
};

struct KeyPredictions {
    std::mutex mutex;
    std::map<std::uint64_t, KeyPrediction> byAddress;
    // (kernel, input, the keys' value or -1 for mixed) -> waits, and how many were predicted right.
    std::map<std::tuple<char, std::uint32_t, int>, std::pair<std::uint64_t, std::uint64_t>> outcomes;
};

KeyPredictions& keyPredictions() {
    static KeyPredictions predictions;
    return predictions;
}

// Measuring aid: what the keys hold once the wait is over (one value throughout, by value, or
// mixed), printed every 10 s: whether the work that wrote them is a plain fill.
void noteKeysAfterSync(std::uint64_t keys, std::size_t count) {
    static std::array<std::uint64_t, 256> uniform{};
    static std::uint64_t mixed = 0, unreadable = 0;
    static auto since = std::chrono::steady_clock::now();
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(keys), count)) {
        ++unreadable;
    } else {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(keys);
        const bool one = AllKeysEqual(bytes + 1, count - 1, bytes[0]);
        if (one) ++uniform[bytes[0]];
        else ++mixed;
        auto& predictions = keyPredictions();
        std::lock_guard lock(predictions.mutex);
        const auto found = predictions.byAddress.find(keys);
        const auto actual = one ? static_cast<int>(bytes[0]) : -1;
        const bool known = found != predictions.byAddress.end() && found->second.bytes >= count;
        if (predictions.outcomes.size() < 64 || predictions.outcomes.contains({known ? found->second.kernel : '-', known ? found->second.input : 0u, actual})) {
            auto& outcome = predictions.outcomes[{known ? found->second.kernel : '-', known ? found->second.input : 0u, actual}];
            ++outcome.first;
            if (known && found->second.value == actual && actual >= 0) ++outcome.second;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - since < std::chrono::seconds(10)) return;
    {
        auto& predictions = keyPredictions();
        std::lock_guard lock(predictions.mutex);
        std::string table;
        for (const auto& [key, outcome] : predictions.outcomes) {
            char text[112];
            std::snprintf(text, sizeof(text), " kernel %c input 0x%08x -> keys %s0x%02x: %llu waits, %llu predicted;", std::get<0>(key), std::get<1>(key), std::get<2>(key) < 0 ? "mixed " : "", std::get<2>(key) < 0 ? 0u : static_cast<unsigned>(std::get<2>(key)), static_cast<unsigned long long>(outcome.first), static_cast<unsigned long long>(outcome.second));
            table += text;
        }
        std::fprintf(stderr, "[key-writer-sync] predictions:%s\n", table.empty() ? " none" : table.c_str());
        predictions.outcomes.clear();
    }
    std::string values;
    for (std::size_t value = 0; value < uniform.size(); ++value) {
        if (uniform[value] == 0) continue;
        char text[48];
        std::snprintf(text, sizeof(text), " 0x%02zx x %llu,", value, static_cast<unsigned long long>(uniform[value]));
        values += text;
    }
    std::fprintf(stderr, "[key-writer-sync] keys after the waits: one value throughout:%s mixed x %llu, unreadable x %llu\n", values.empty() ? " none," : values.c_str(), static_cast<unsigned long long>(mixed), static_cast<unsigned long long>(unreadable));
    uniform = {};
    mixed = unreadable = 0;
    since = now;
}

}

void NoteKeyKernelDispatch(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads, const std::array<std::uint32_t, 3>& groups) {
    static constexpr std::array<std::uint32_t, 292> kernelA{0xbfa00003u, 0xd746000au, 0x0405060au, 0xd7460004u, 0x04010609u, 0xbe8c0301u, 0xbe8d0302u, 0xbe8e0303u, 0x34121483u, 0xd5480000u, 0x025d0d0au, 0xd7460007u, 0x02210d04u, 0x34101484u, 0xd548000du, 0x025d0704u, 0x2c161281u, 0x2c041282u, 0xd569000cu, 0x00020000u, 0x34061485u, 0x340c1486u, 0x361e1688u, 0x361c04a0u, 0x3400088au, 0x34021488u, 0x2c0a0e89u, 0xd771000fu, 0x043e0482u, 0xd771000eu, 0x043a148au, 0x2c140e83u, 0xd7470002u, 0x0232190du, 0xd7470005u, 0x02321905u, 0xbe8f0304u, 0xd771000du, 0x043a16ffu, 0x00000080u, 0xd771000eu, 0x043e12a0u, 0x2c160e84u, 0xd7710009u, 0x03fe1491u, 0x00000300u, 0xd771000cu, 0x03fe1491u, 0x00000900u, 0xd7710010u, 0x03fe1491u, 0x00000100u, 0x2c0e0e82u, 0xd7710001u, 0x043602ffu, 0x00000f00u, 0xd771000du, 0x043a10ffu, 0x00000080u, 0xd7710008u, 0x03fe1491u, 0x00000b00u, 0x34140886u, 0xd7710009u, 0x04261684u, 0xd771000eu, 0x04321684u, 0xd771000cu, 0x043606ffu, 0x00000200u, 0xd771000fu, 0x04221684u, 0x2c1a1483u, 0xd771000bu, 0x04421684u, 0xd7710010u, 0x04260ec0u, 0xd771000eu, 0x043a0ec0u, 0xd771000fu, 0x043e0ec0u, 0xd7710008u, 0x04320cffu, 0x00000800u, 0x34060887u, 0xd771000cu, 0x03fe1a90u, 0x00000a00u, 0x34080888u, 0xd7710011u, 0x043e00ffu, 0x00000400u, 0xd7710006u, 0x042e0ec0u, 0x2c0e1484u, 0xd771000bu, 0x03fe1a90u, 0x00000800u, 0xbe800305u, 0xbe810306u, 0xd7710009u, 0x041a00ffu, 0x00000400u, 0x361e0e84u, 0xd7710006u, 0x043a00ffu, 0x00000400u, 0xd771000eu, 0x044200ffu, 0x00000400u, 0xd7710010u, 0x03fe1a90u, 0x00000200u, 0xd7450009u, 0x04161301u, 0xd7710012u, 0x043e1a90u, 0x2c1a1482u, 0xd7450007u, 0x04161d01u, 0x361c14c0u, 0xd771000au, 0x042214c0u, 0xd7450006u, 0x04160d01u, 0xd7710012u, 0x044a1ac0u, 0xd7450005u, 0x04162301u, 0x36221ac0u, 0xd771000au, 0x042a06ffu, 0x00000100u, 0xd772000du, 0x043a1090u, 0xbe820307u, 0xbe830308u, 0xd7720013u, 0x04461f10u, 0xd772000bu, 0x04461f0bu, 0xd772000cu, 0x04461f0cu, 0xd772000fu, 0x043a1084u, 0xd771000au, 0x042a08ffu, 0x00000400u, 0xd7710011u, 0x044a00ffu, 0x00000400u, 0xd7710010u, 0x042e00ffu, 0x00000400u, 0xd771000cu, 0x043200ffu, 0x00000400u, 0xd7710000u, 0x044e00ffu, 0x00000400u, 0xd7720012u, 0x043a1094u, 0xd771000eu, 0x043e06ffu, 0x00000100u, 0xd745000bu, 0x040a2301u, 0xd7450008u, 0x040a2101u, 0xd7450000u, 0x040a0101u, 0xd745000cu, 0x040a1901u, 0xd7710001u, 0x043606ffu, 0x00000100u, 0xd771000du, 0x044a06ffu, 0x00000100u, 0xe0002000u, 0x80030309u, 0xe0002000u, 0x80030000u, 0xe0002000u, 0x80030505u, 0xd7710001u, 0x040608ffu, 0x00000400u, 0xd7710009u, 0x043608ffu, 0x00000400u, 0xd7710004u, 0x043a08ffu, 0x00000400u, 0x4a1a0302u, 0x4a1c1302u, 0x4a120902u, 0xe0002000u, 0x80030107u, 0xe0002000u, 0x8003040cu, 0x4a0e1502u, 0x2c141a81u, 0x2c121281u, 0x2c180e81u, 0xe0002000u, 0x8003020bu, 0xe0002000u, 0x80030706u, 0xe0002000u, 0x80030608u, 0x2c161c81u, 0xbf8c3f72u, 0xe0142000u, 0x8000020cu, 0xe0142000u, 0x80000009u, 0xbf8c3f70u, 0xe0142000u, 0x8000060au, 0xe0142000u, 0x8000040bu, 0xbf810000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x30306c73u, 0x00000085u, 0x000000a0u, 0x00000000u, 0x00102557u, 0x0401a600u, 0x0b100a10u, 0x40100c10u, 0x4b204240u, 0x4d104c10u, 0x6f104e10u, 0x7a107960u, 0x00108510u, 0x00006e00u, 0x21200f64u, 0x25402410u, 0x65000010u, 0x40241021u, 0x21640000u, 0x00402410u, 0x10296100u, 0x28600000u, 0x00102950u, 0x1da08100u, 0x80200100u, 0x2117acfdu, 0x00560100u, 0x4a006a51u, 0x6f436a14u, 0x6c106c04u, 0x0a382472u, 0x10087600u, 0x35247a76u, 0x0183157fu, 0x01080802u, 0xa80b017fu, 0x00049846u, 0x80800d5du, 0xee2040e0u, 0x0bc198fbu, 0x65726162u, 0x746f6f66u, 0x9fe61901u, 0x00000000u, 0x00000001u, 0x000003b4u, 0x00000000u, 0x000000a0u, 0xb3e3c624u, 0xb8263deeu, 0x00000000u, 0x00000000u};
    static constexpr std::array<std::uint32_t, 12> kernelB{0xd7460004u, 0x04010c08u, 0xf4280302u, 0xfa000000u, 0xbf8cc07fu, 0x7e00020cu, 0x7e02020du, 0x7e04020eu, 0x7e06020fu, 0xe01c2000u, 0x80000004u, 0xbf810000u};
    static_cast<void>(groups);
    KeyPrediction prediction;
    std::uint64_t address = 0;
    if (userData.size() >= 9 && numThreads == std::array<std::uint32_t, 3>{8u, 8u, 1u} && code.size() >= kernelA.size() && std::equal(kernelA.begin(), kernelA.end(), code.begin())) {
        // The value in user word 0, the keys as a byte buffer in words 1-4.
        const auto stride = (userData[2] >> 16u) & 0x3fffu;
        address = userData[1] | (static_cast<std::uint64_t>(userData[2] & 0xffffu) << 32u);
        prediction = {static_cast<std::uint64_t>(stride == 0 ? 1u : stride) * userData[3], static_cast<int>((userData[0] << 4u) & 0xffu), 'A', userData[0]};
    } else if (userData.size() >= 8 && numThreads == std::array<std::uint32_t, 3>{64u, 1u, 1u} && code.size() >= kernelB.size() && std::equal(kernelB.begin(), kernelB.end(), code.begin())) {
        // A fill of 16-byte elements (words 0-3) from a one-element source (words 4-7).
        const auto stride = (userData[1] >> 16u) & 0x3fffu;
        const auto sourceStride = (userData[5] >> 16u) & 0x3fffu;
        address = userData[0] | (static_cast<std::uint64_t>(userData[1] & 0xffffu) << 32u);
        const auto source = userData[4] | (static_cast<std::uint64_t>(userData[5] & 0xffffu) << 32u);
        if (stride != 16 || sourceStride != 16 || userData[6] != 1 || !GuestMemory::Accessible(reinterpret_cast<const void*>(source), 16)) return;
        std::array<std::uint8_t, 16> element{};
        std::memcpy(element.data(), reinterpret_cast<const void*>(source), element.size());
        std::uint32_t first = 0;
        std::memcpy(&first, element.data(), sizeof(first));
        const bool uniform = AllKeysEqual(element.data() + 1, element.size() - 1, element[0]);
        prediction = {static_cast<std::uint64_t>(stride) * userData[2], uniform ? static_cast<int>(element[0]) : -1, 'B', first};
    } else {
        return;
    }
    auto& predictions = keyPredictions();
    std::lock_guard lock(predictions.mutex);
    if (predictions.byAddress.size() < 256 || predictions.byAddress.contains(address)) predictions.byAddress[address] = prediction;
}

void ReportKeyWriterDispatch(std::uint64_t codeAddress, std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads, const std::array<std::uint32_t, 3>& groups) {
    static std::atomic<unsigned> reports{0};
    if (reports.load(std::memory_order_relaxed) >= 8 || userData.size() < 2) return;
    auto& waited = waitedKeys();
    std::lock_guard lock(waited.mutex);
    if (waited.addresses.empty()) return;
    static std::set<std::uint64_t> reported;
    for (std::size_t i = 0; i + 1 < userData.size(); ++i) {
        const auto address = userData[i] | (static_cast<std::uint64_t>(userData[i + 1] & 0xffffu) << 32u);
        if (!waited.addresses.contains(address)) continue;
        if (!reported.insert(codeAddress).second && reports.load(std::memory_order_relaxed) >= 3) return;
        if (reports.fetch_add(1) >= 8) return;
        std::fprintf(stderr, "[key-writer-sync] compute shader 0x%llx (%ux%ux%u threads, %ux%ux%u groups) is given the DCC keys 0x%llx in user data word %zu; user data:", static_cast<unsigned long long>(codeAddress), numThreads[0], numThreads[1], numThreads[2], groups[0], groups[1], groups[2], static_cast<unsigned long long>(address), i);
        for (const auto word : userData) std::fprintf(stderr, " %08x", word);
        std::fprintf(stderr, "\n[key-writer-sync]   code (%zu words):", code.size());
        for (std::size_t word = 0; word < code.size() && word < 48; ++word) std::fprintf(stderr, " %08x", code[word]);
        std::fprintf(stderr, "\n");
        return;
    }
}

std::optional<DccKeys> WaitForKeyWriters(const GuestTextureResource& resource, std::uint64_t guestBytes) {
    const auto count = static_cast<std::size_t>(guestBytes / KeyBytes);
    const auto metaAddress = resource.dccAddress;
    if (metaAddress == 0 || count == 0 || !GuestMemory::GpuMutex().HeldByThisThread()) return std::nullopt;
    auto* recorder = Recorder::Active();
    if (recorder == nullptr || !recorder->PendingWriteOverlaps(metaAddress, count) || PendingStoreKeys(*recorder, metaAddress, count).has_value()) return std::nullopt;
    if (PendingFillsLeaveUncompressed(*recorder, resource, count)) return DccKeys::Uncompressed;
    noteKeyWriterSync(*recorder, metaAddress, count);
    Recorder::CountSync(2);
    recorder->SyncThrough(metaAddress, count);
    noteKeysAfterSync(metaAddress, count);
    return std::nullopt;
}

}
