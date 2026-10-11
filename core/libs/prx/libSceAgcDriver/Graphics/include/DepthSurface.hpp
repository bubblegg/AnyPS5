#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Texture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
void ClearDepthSurfaces(VkDevice device);
bool DepthSurfaceAt(std::uint64_t address);
void RetireDepthSurfacesAt(std::uint64_t address);
std::uint64_t HtileDepthClearAddress(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads);
void NoteHtileDepthClear(std::uint64_t htileAddress);
// Reports the first few compute dispatches whose user data names a depth plane's HTILE.
void ReportHtileDispatch(std::uint64_t codeAddress, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads);
// Whether a depth plane in use has its HTILE at this address.
bool DepthHtileAt(std::uint64_t address);
// A buffer fill by the title: one that covers a depth plane's HTILE with a word whose ZMASK is zero is
// the title's fast depth clear, and clears that plane when it is next bound.
void NoteHtileFill(std::uint64_t base, std::uint64_t bytes, std::uint32_t word);
// Counts a draw that clears depth through DB_RENDER_CONTROL, for the report below.
void NoteDepthClearDraw();
// At each flip: reports now and then how the title cleared depth, and under APS5_CLEAR_DEPTH_ON_FLIP
// clears the depth plane of every surface of the device (a debug aid for ghosting).
void DepthSurfacesAtFlip(const Context& context);
bool DepthSurfaceHolds(const Context& context, const GuestTextureResource& resource, const Texture* texture);
void NoteDepthMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern);
VkImageAspectFlags HtileFillClears(std::uint32_t pattern, bool stencilInHtile);
std::uint64_t HtileMetadataBytes(VkExtent2D extent);
bool HtileFillCovers(std::uint64_t htile, VkExtent2D extent, std::uint64_t address, std::size_t bytes);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

}

#endif
