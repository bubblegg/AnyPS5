#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <atomic>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Module/EhFrame.hpp"
#include "prx/libkernel/Module/ModuleArgs.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#else
#include <string_view>
#include "prx/libc/include/specifics/linux/HostMapping.hpp"
#endif

#ifdef _WIN32
namespace {
void FillGuestUnwindInfo(const std::uint8_t* base, ModuleInfoForUnwind* info) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: image without a DOS header");
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: image without an NT header");
  EhFrame::Tables tables;
  if (!EhFrame::ReadEhmeta(reinterpret_cast<std::uintptr_t>(base), *nt, "sceKernelGetModuleInfoForUnwind", tables)) return;
  info->eh_frame_hdr_addr = tables.header;
  info->eh_frame_addr = tables.frames;
  info->eh_frame_size = tables.framesSize;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(base);
  info->seg0_size = nt->OptionalHeader.SizeOfImage;
}
}
#elif !defined(__APPLE__)
extern "C" int APS5_VABI sceKernelGetModuleInfoFromAddr(std::uint64_t address, int flags, ModuleInfoEx* info);

namespace {
struct ImageSearch {
  std::uint64_t address;
  bool relinked;
};

int FindRelinkedImage(dl_phdr_info* image, std::size_t, void* data) {
  auto& search = *static_cast<ImageSearch*>(data);
  bool contains = false;
  for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
    const auto& header = image->dlpi_phdr[index];
    const auto start = image->dlpi_addr + header.p_vaddr;
    if (header.p_type == PT_LOAD && search.address >= start && search.address - start < header.p_memsz) contains = true;
  }
  if (!contains) return 0;
  const std::string_view name = image->dlpi_name != nullptr ? image->dlpi_name : "";
  search.relinked = name.empty() || name.ends_with(".guest.prx");
  return 1;
}

bool IsRelinkedImage(std::uint64_t address) {
  ImageSearch search{address, false};
  dl_iterate_phdr(FindRelinkedImage, &search);
  return search.relinked;
}

void FillGuestUnwindInfo(std::uint64_t address, ModuleInfoForUnwind* info) {
  ModuleInfoEx module{};
  module.st_size = sizeof(ModuleInfoEx);
  if (sceKernelGetModuleInfoFromAddr(address, 2, &module) != 0)
    throw std::runtime_error("sceKernelGetModuleInfoForUnwind: failed to query guest module information");
  info->eh_frame_hdr_addr = module.eh_frame_hdr_addr;
  info->eh_frame_addr = module.eh_frame_addr;
  info->eh_frame_size = module.eh_frame_size;
}
}
#endif

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags);
void* LoadStartModule_nid_no_patch(const char* path, std::size_t args, const void* argp, int* result);
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name);
int APS5_VABI dlclose_nid_postfix(void* handle);
}

namespace {
constexpr int kRtldNow = 2;
}

extern "C" {

int APS5_VABI sceKernelDlsym(KernelModule handle, const char* symbol, void** addr) {
 if (!symbol || !addr) return SCE_KERNEL_ERROR_EFAULT;
 void* module = handle == 0 ? dlopen_nid_postfix(nullptr, kRtldNow) : reinterpret_cast<void*>(static_cast<intptr_t>(handle));
 void* found = dlsym_nid_postfix(module, symbol);
 if (handle == 0 && module) dlclose_nid_postfix(module);
 if (!found) return SCE_KERNEL_ERROR_ESRCH;
 *addr = found;
 return 0;
}

}

#ifdef _WIN32
namespace {
// Debug aid: APS5_TRACE_THROWS=1 names what a title throws. The unwinder asks for module info while a
// C++ exception is raised; the exception's _Unwind_Exception (class "CLNGC++\0" from libc++abi,
// "GNUCC++\0" from libsupc++) is referenced from the raising frames above, and for IL2CPP the thrown
// object is an Il2CppExceptionWrapper whose managed exception names its class and message. Every
// distinct class and message is counted; the counts are printed every 5 s.
bool ReadableRange(std::uint64_t address, std::size_t bytes) {
  if (address < 0x10000 || address > 0x7fffffffffffull - bytes) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return false;
  if ((memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
  return address + bytes <= reinterpret_cast<std::uint64_t>(memory.BaseAddress) + memory.RegionSize;
}

template <class T>
bool ReadValue(std::uint64_t address, T& value) {
  if (!ReadableRange(address, sizeof(T))) return false;
  std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
  return true;
}

std::string ReadCString(std::uint64_t address) {
  std::string text;
  for (std::size_t i = 0; i < 128; ++i) {
    char c = 0;
    if (!ReadValue(address + i, c) || c == 0) break;
    text.push_back(c);
  }
  return text;
}

std::string DescribeManagedException(std::uint64_t exception) {
  std::uint64_t klass = 0, name = 0, space = 0, message = 0;
  if (!ReadValue(exception, klass) || !ReadValue(klass + 0x10, name) || !ReadValue(klass + 0x18, space)) return "?";
  std::string text = ReadCString(space);
  if (!text.empty()) text += ".";
  text += ReadCString(name);
  std::int32_t length = 0;
  if (ReadValue(exception + 0x18, message) && message != 0 && ReadValue(message + 0x10, length) && length > 0) {
    text += ": ";
    for (std::int32_t i = 0; i < std::min(length, 160); ++i) {
      std::uint16_t unit = 0;
      if (!ReadValue(message + 0x14 + static_cast<std::uint64_t>(i) * 2, unit)) break;
      text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
  }
  return text;
}

void TraceThrow(std::uint64_t pc) {
  static const bool enabled = std::getenv("APS5_TRACE_THROWS") != nullptr;
  if (!enabled) return;
  thread_local std::uint64_t lastHeader = 0;
  static std::mutex countsLock;
  static std::map<std::string, std::uint64_t> counts;
  static std::uint64_t lastReport = 0;
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  volatile std::uint64_t marker = 0;
  const auto start = reinterpret_cast<std::uint64_t>(&marker) & ~7ull;
  const auto end = std::min<std::uint64_t>(high, start + 0x8000);
  for (auto slot = start; slot + 8 <= end; slot += 8) {
    const auto candidate = *reinterpret_cast<const std::uint64_t*>(slot);
    if ((candidate & 7) != 0 || candidate < 0x10000 || (candidate >= low && candidate < high)) continue;
    std::uint64_t cls = 0;
    if (!ReadValue(candidate, cls)) continue;
    if ((cls & ~0xffull) != 0x434C4E47432B2B00ull && (cls & ~0xffull) != 0x474E5543432B2B00ull) continue;
    if (candidate == lastHeader) return;
    lastHeader = candidate;
    std::uint64_t wrapped = 0;
    const auto description = ReadValue(candidate + 0x20, wrapped) && wrapped != 0 ? DescribeManagedException(wrapped) : std::string("?");
    std::lock_guard lock(countsLock);
    if (++counts[description] == 1) {
      HMODULE module = nullptr;
      char path[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &module) && module) GetModuleFileNameA(module, path, sizeof(path));
      const char* leaf = std::strrchr(path, '\\');
      std::fprintf(stderr, "[throw] new: %s (first frame %s+0x%llx)\n", description.c_str(), leaf ? leaf + 1 : path, static_cast<unsigned long long>(pc - reinterpret_cast<std::uint64_t>(module)));
    }
    const auto now = GetTickCount64();
    if (now - lastReport >= 5000) {
      lastReport = now;
      std::vector<std::pair<std::uint64_t, std::string>> top;
      for (const auto& [text, count] : counts) top.emplace_back(count, text);
      std::sort(top.rbegin(), top.rend());
      std::fprintf(stderr, "[throw] totals:\n");
      for (std::size_t i = 0; i < top.size() && i < 8; ++i) std::fprintf(stderr, "[throw]   %llu x %s\n", static_cast<unsigned long long>(top[i].first), top[i].second.c_str());
    }
    return;
  }
}
}  // namespace
#endif

extern "C" {

int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info) {
  (void)flags;
  if (!info) return SCE_KERNEL_ERROR_EFAULT;
#ifdef _WIN32
  TraceThrow(addr);
  // Guest images (their .ehmeta) answer for their whole extent: the stack walks of the title's
  // runtime (many per frame, not only throws) reach them without a VirtualQuery per frame. Images
  // stay mapped while their frames can be walked.
  static std::shared_mutex guestRangesLock;
  static std::map<std::uint64_t, std::pair<std::uint64_t, ModuleInfoForUnwind>> guestRanges;
  {
    std::shared_lock lock(guestRangesLock);
    if (auto it = guestRanges.upper_bound(addr); it != guestRanges.begin() && addr < (--it)->second.first) {
      *info = it->second.second;
      return 0;
    }
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return SCE_KERNEL_ERROR_ESRCH;
  info->st_size = sizeof(ModuleInfoForUnwind);
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
  info->seg0_size = mbi.RegionSize;
  // An image's answer never changes while it is mapped, and C++ unwinding asks once per frame of
  // every throw: the eh_frame walk (millions of bytes for a large module) and the mapped-file
  // query are made once per image. Other regions are described per call, as before.
  struct ImageUnwind {
    ModuleInfoForUnwind info;
    bool guest;
  };
  static std::shared_mutex imagesLock;
  static std::unordered_map<const void*, ImageUnwind> images;
  const auto describeName = [&](ModuleInfoForUnwind* target) {
    char path[4096] = {};
    DWORD len = GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, path, sizeof(path) - 1);
    path[len] = '\0';
    std::strncpy(target->name, path, sizeof(target->name) - 1);
    target->name[sizeof(target->name) - 1] = '\0';
  };
  // Debug aid: APS5_TRACE_UNWIND_INFO=1 counts the lookups every 5 s by region type and names the
  // first regions outside images.
  static const bool trace = std::getenv("APS5_TRACE_UNWIND_INFO") != nullptr;
  if (trace) {
    static std::atomic<std::uint64_t> images_{0}, privates{0}, mapped{0};
    static std::atomic<std::int64_t> lastReport{0};
    static std::atomic<int> named{0};
    (mbi.Type == MEM_IMAGE ? images_ : mbi.Type == MEM_PRIVATE ? privates : mapped).fetch_add(1, std::memory_order_relaxed);
    if (mbi.Type != MEM_IMAGE && named.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[unwind-info] 0x%llx in %s region 0x%llx+0x%llx (allocation 0x%llx, protect 0x%lx)\n", static_cast<unsigned long long>(addr), mbi.Type == MEM_PRIVATE ? "private" : "mapped", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)), static_cast<unsigned long long>(mbi.RegionSize), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)), mbi.Protect);
    const auto now = static_cast<std::int64_t>(GetTickCount64());
    auto last = lastReport.load(std::memory_order_relaxed);
    if (now - last >= 5000 && lastReport.compare_exchange_strong(last, now)) std::fprintf(stderr, "[unwind-info] lookups so far: %llu in images, %llu private, %llu mapped\n", static_cast<unsigned long long>(images_.load()), static_cast<unsigned long long>(privates.load()), static_cast<unsigned long long>(mapped.load()));
  }
  if (mbi.Type != MEM_IMAGE) {
    // Private memory maps no file: the name query would only fail, a system call per frame.
    if (mbi.Type == MEM_MAPPED) describeName(info);
    else info->name[0] = '\0';
    return 0;
  }
  {
    std::shared_lock lock(imagesLock);
    if (const auto found = images.find(mbi.AllocationBase); found != images.end()) {
      const auto seg0 = std::pair{info->seg0_addr, info->seg0_size};
      *info = found->second.info;
      if (!found->second.guest) std::tie(info->seg0_addr, info->seg0_size) = seg0;
      return 0;
    }
  }
  ImageUnwind entry{*info, false};
  FillGuestUnwindInfo(static_cast<const std::uint8_t*>(mbi.AllocationBase), &entry.info);
  entry.guest = entry.info.eh_frame_hdr_addr != 0;
  describeName(&entry.info);
  {
    std::unique_lock lock(imagesLock);
    images.emplace(mbi.AllocationBase, entry);
  }
  if (entry.guest) {
    std::unique_lock lock(guestRangesLock);
    guestRanges.emplace(entry.info.seg0_addr, std::pair{entry.info.seg0_addr + entry.info.seg0_size, entry.info});
  }
  *info = entry.info;
  return 0;
#elif defined(__APPLE__)
  struct Search {
    std::uint64_t address;
    ModuleInfoForUnwind* info;
    bool found;
  } search {addr, info, false};
  dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
    auto& search = *static_cast<Search*>(data);
    const Elf64_Phdr* first = nullptr;
    const Elf64_Phdr* frames = nullptr;
    bool contains = false;
    for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
      const auto& header = image->dlpi_phdr[index];
      const auto start = image->dlpi_addr + header.p_vaddr;
      if (header.p_type == PT_LOAD && first == nullptr) first = &header;
      if (header.p_type == PT_LOAD && search.address >= start && search.address - start < header.p_memsz) contains = true;
      if (header.p_type == PT_GNU_EH_FRAME) frames = &header;
    }
    if (!contains) return 0;
    auto* info = search.info;
    info->st_size = sizeof(ModuleInfoForUnwind);
    std::strncpy(info->name, image->dlpi_name != nullptr ? image->dlpi_name : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->eh_frame_hdr_addr = frames != nullptr ? image->dlpi_addr + frames->p_vaddr : 0;
    info->eh_frame_addr = 0;
    info->eh_frame_size = 0;
    info->seg0_addr = image->dlpi_addr + first->p_vaddr;
    info->seg0_size = first->p_memsz;
    search.found = true;
    return 1;
  }, &search);
  if (search.found) return 0;
  Dl_info image {};
  if (!dladdr(reinterpret_cast<void*>(addr), &image) || image.dli_fbase == nullptr) return SCE_KERNEL_ERROR_ESRCH;
  unsigned long textSize = 0;
  getsegmentdata(static_cast<const mach_header_64*>(image.dli_fbase), "__TEXT", &textSize);
  info->st_size = sizeof(ModuleInfoForUnwind);
  std::strncpy(info->name, image.dli_fname != nullptr ? image.dli_fname : "", sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(image.dli_fbase);
  info->seg0_size = textSize;
  return 0;
#else
  const auto mapping = HostMapping::Find(addr, "sceKernelGetModuleInfoForUnwind");
  if (!mapping) return SCE_KERNEL_ERROR_ESRCH;
  info->st_size = sizeof(ModuleInfoForUnwind);
  std::strncpy(info->name, mapping->path.c_str(), sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = mapping->start;
  info->seg0_size = mapping->end - mapping->start;
  if (IsRelinkedImage(addr)) FillGuestUnwindInfo(addr, info);
  return 0;
#endif
}

namespace {

thread_local PendingModuleArgs pendingModuleArgs;
thread_local int pendingModuleInitResult = 0;

}

extern "C" {

const void* __aps5_get_pending_module_args_nid_no_patch() {
    return &pendingModuleArgs;
}

void __aps5_set_module_init_result_nid_no_patch(int result) {
    pendingModuleInitResult = result;
}

}

KernelModule APS5_VABI sceKernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags, const KernelLoadModuleOpt* opt, int* res) {
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 if (!module_file_name) return static_cast<KernelModule>(SCE_KERNEL_ERROR_EFAULT);
 // Modules converted by this tree's relinker (--guest-module-dir) take their arguments through
 // DT_INIT and run their deferred initializers in LoadStartModule; the pending-argument block
 // serves modules whose startup asks for it.
 pendingModuleArgs = {args, argp};
 pendingModuleInitResult = 0;
 int started = 0;
 void* handle = LoadStartModule_nid_no_patch(module_file_name, args, argp, &started);
 if (pendingModuleInitResult != 0) started = pendingModuleInitResult;
 pendingModuleArgs = {};
 pendingModuleInitResult = 0;
 if (res) *res = started;
 if (!handle) return static_cast<KernelModule>(SCE_KERNEL_ERROR_ENOENT);
 return static_cast<KernelModule>(reinterpret_cast<intptr_t>(handle));
}

int APS5_VABI sceKernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags, const KernelUnloadModuleOpt* opt, int* res) {
 (void)args;
 (void)argp;
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 return dlclose_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle))) == 0 ? 0 : SCE_KERNEL_ERROR_ESRCH;
}

}

extern "C" {

int APS5_VABI __elf_phdr_match_addr_nid_postfix(dl_phdr_info* phdrInfo, void* addr) {
    if (phdrInfo == nullptr) throw std::invalid_argument("__elf_phdr_match_addr: phdr_info is null");
    const auto address = reinterpret_cast<std::uintptr_t>(addr);
    for (std::uint16_t i = 0; i < phdrInfo->dlpi_phnum; ++i) {
        const Elf64_Phdr& header = phdrInfo->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        const std::uintptr_t begin = phdrInfo->dlpi_addr + header.p_vaddr;
        if (begin <= address && address + sizeof(addr) < begin + header.p_memsz) return 1;
    }
    return 0;
}

// unknown signature
std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
