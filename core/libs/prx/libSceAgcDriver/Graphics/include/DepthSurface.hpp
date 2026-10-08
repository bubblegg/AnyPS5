#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Texture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
void ClearDepthSurfaces(VkDevice device);
// A DB_RENDER_CONTROL clear of the surface's depth and/or stencil plane, recorded in order.
void ClearDepthSurface(const Context& context, const DepthTarget& target, bool depth, bool stencil);
// Debug aid (APS5_CLEAR_DEPTH_ON_FLIP): clears the depth plane of every surface of the device.
void ClearDepthPlanes(const Context& context);
bool DepthSurfaceAt(std::uint64_t address);
// The title uses a surface's memory for something else (a color target): the surface is stale.
void RetireDepthSurfacesAt(std::uint64_t address, const char* why);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

}

#endif
