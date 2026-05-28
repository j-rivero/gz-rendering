/*
 * Copyright (C) 2024 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include "gz/rendering/o3de/O3deCamera.hh"
#include "gz/rendering/o3de/O3deRenderTarget.hh"

#if defined(GZ_O3DE_INTEROP_BUILD)
#include <vector>

#include <gz/common/Console.hh>

#include "gz/rendering/o3de/O3deRenderEngine.hh"
#include "O3deBackend.hh"
#include "O3deVkImport.hh"  // pulls in <vulkan/vulkan.h>, O3deInteropImport
#endif

using namespace gz;
using namespace rendering;

#if defined(GZ_O3DE_INTEROP_BUILD)
namespace
{
  /// \brief First queue family with graphics support on \p _phys. Qt's QRhi
  /// uses the first graphics(+present) family, and this Qt version exposes no
  /// queue-family-index resource, so we reproduce that choice. Matches the
  /// queue Qt hands us; the importer records its ownership-acquire barrier and
  /// command pool against this family.
  uint32_t FirstGraphicsFamily(VkPhysicalDevice _phys)
  {
    uint32_t n = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(_phys, &n, qf.data());
    for (uint32_t i = 0u; i < n; ++i)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        return i;
    return 0u;
  }
}  // namespace
#endif

/// \brief Imported-image state for the native Vulkan->Vulkan display path.
/// Opaque to the public header; empty in non-interop builds.
class O3deCamera::O3deCameraInterop
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  public: O3deVkDeviceContext ctx;     //!< Qt's device the image is imported on.
  public: O3deVkImportedImage img;     //!< The VkImage aliasing Atom's memory.
  public: bool imported = false;       //!< Import succeeded (latched).
  public: bool acquired = false;       //!< Ownership/layout acquired (once).
  public: int warnCount = 0;           //!< Not-ready warnings emitted (rate cap).
#endif
};

//////////////////////////////////////////////////
O3deCamera::O3deCamera()
{
}

//////////////////////////////////////////////////
O3deCamera::~O3deCamera()
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  if (this->interop && this->interop->imported)
    O3deVkDestroyImported(this->interop->ctx, &this->interop->img);
#endif
}

//////////////////////////////////////////////////
void O3deCamera::Render()
{
  if (this->renderTexture)
    this->renderTexture->Render();
}

//////////////////////////////////////////////////
void O3deCamera::RenderTextureMetalId(void *_textureIdPtr) const
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  if (!_textureIdPtr)
    return;
  if (!this->interop)
    this->interop = std::make_unique<O3deCameraInterop>();
  O3deCameraInterop &st = *this->interop;

  // Import Atom's exported colour image onto Qt's injected VkDevice. Retried
  // until it succeeds (the injected device and the exportable image may not be
  // ready on the first call from gz-gui); latched once imported. The warning is
  // rate-limited so a not-yet-ready state does not spam per frame.
  if (!st.imported)
  {
    auto *eng = O3deRenderEngine::Instance();
    auto dev = static_cast<VkDevice>(eng->QtVulkanDevice());
    auto phys = static_cast<VkPhysicalDevice>(eng->QtVulkanPhysicalDevice());
    auto queue = static_cast<VkQueue>(eng->QtVulkanGraphicsQueue());
    auto inst = static_cast<VkInstance>(eng->QtVulkanInstance());
    O3deInteropImport import;
    const bool haveDevice = dev && phys && queue;
    const bool haveImage =
        haveDevice && O3deBackend::Instance().GetInteropImport(import);
    if (!haveImage)
    {
      if (st.warnCount++ < 3)
      {
        gzwarn << "[gz-o3de] RenderTextureMetalId: "
               << (!haveDevice ? "Qt did not inject a Vulkan device (is the "
                                 "Vulkan GUI backend active?)"
                               : "backend has no exportable image yet "
                                 "(interop not ready)")
               << " -- will retry" << std::endl;
      }
      return;  // no handle this frame; try again next call
    }
    st.ctx.instance = inst;
    st.ctx.physicalDevice = phys;
    st.ctx.device = dev;
    st.ctx.queue = queue;
    st.ctx.queueFamily = FirstGraphicsFamily(phys);
    if (O3deVkImportImage(st.ctx, import, &st.img))
    {
      st.imported = true;
      gzmsg << "[gz-o3de] imported Atom image onto Qt's VkDevice (VkImage="
            << reinterpret_cast<void *>(st.img.image) << ", " << st.img.width
            << "x" << st.img.height << ") -- native Vulkan->Vulkan display"
            << std::endl;
    }
    else
    {
      gzerr << "[gz-o3de] failed to import Atom image onto Qt's VkDevice"
            << std::endl;
      return;
    }
  }

  if (st.imported)
    *static_cast<VkImage *>(_textureIdPtr) = st.img.image;
#else
  (void)_textureIdPtr;
#endif
}

//////////////////////////////////////////////////
void O3deCamera::PrepareForExternalSampling()
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  if (!this->interop || !this->interop->imported)
    return;
  O3deCameraInterop &st = *this->interop;
  // Static probe image: acquire ownership from the producer and settle the
  // layout once. A live render-into target (Phase 2) must do this every frame,
  // gated on the exported render-finished semaphore.
  if (!st.acquired)
  {
    if (O3deVkAcquireFromProducer(st.ctx, st.img,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
      st.acquired = true;
  }
#endif
}

//////////////////////////////////////////////////
RenderTargetPtr O3deCamera::RenderTarget() const
{
  return this->renderTexture;
}

//////////////////////////////////////////////////
void O3deCamera::Init()
{
  BaseCamera::Init();
  this->CreateRenderTexture();
  // Reset() seeds the default image format (PF_R8G8B8) and other camera
  // defaults, matching Ogre2Camera::Init(). Without it the format stays
  // PF_UNKNOWN(0), which gz-common rejects ("Invalid PixelFormat value: 0").
  this->Reset();
}

//////////////////////////////////////////////////
void O3deCamera::CreateRenderTexture()
{
  this->renderTexture = O3deRenderTargetPtr(new O3deRenderTarget);
  // Let the target read this camera's pose/projection and scene at Copy() time.
  this->renderTexture->camera = this;
}
