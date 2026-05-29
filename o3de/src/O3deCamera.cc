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
#include <unistd.h>  // close()

#include <cstdio>
#include <cstdlib>
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
  public: bool live = false;           //!< Producer renders into it each frame.
  public: uint64_t importedGeneration = 0u;  //!< Producer image gen we imported.
  public: uint64_t semaphoreWaitValue = 0u;  //!< Timeline value to wait on (#26).
  public: int warnCount = 0;           //!< Not-ready warnings emitted (rate cap).
  /// \brief Previous imports kept alive after a re-import (producer resize). Qt
  /// may still have an in-flight frame sampling the old VkImage; freeing it now
  /// loses Qt's device. Freed only when the camera is destroyed. Resizes are
  /// rare, so this bounded retention is cheap.
  public: std::vector<O3deVkImportedImage> retiredImports;
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
  // Only destroy imports if Qt's device is still alive and unchanged from the one
  // we imported on; otherwise the VkImage already died with that device and
  // vkDestroyImage on the stale handle would abort (see RenderTextureMetalId).
  // Frees the current import plus any retired (resize) imports kept alive for Qt.
  if (this->interop)
  {
    auto *eng = O3deRenderEngine::Instance();
    auto dev = eng ? static_cast<VkDevice>(eng->QtVulkanDevice()) : nullptr;
    if (dev && dev == this->interop->ctx.device)
    {
      if (this->interop->imported)
        O3deVkDestroyImported(this->interop->ctx, &this->interop->img);
      for (auto &old : this->interop->retiredImports)
        O3deVkDestroyImported(this->interop->ctx, &old);
    }
  }
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

  // Import Atom's exported colour image onto Qt's injected VkDevice, and
  // re-import whenever the producer (re)creates it (probe -> camera size, or a
  // resize), signalled by a changed generation. Retried until the device + image
  // are ready; the warning is rate-limited so a not-yet-ready state does not
  // spam per frame. GetInteropImport() dups a fresh OS FD on each success: a
  // successful O3deVkImportImage() transfers ownership of that FD to the device;
  // otherwise we must close it here.
  auto *eng = O3deRenderEngine::Instance();
  auto dev = static_cast<VkDevice>(eng->QtVulkanDevice());
  auto phys = static_cast<VkPhysicalDevice>(eng->QtVulkanPhysicalDevice());
  auto queue = static_cast<VkQueue>(eng->QtVulkanGraphicsQueue());
  auto inst = static_cast<VkInstance>(eng->QtVulkanInstance());
  const bool haveDevice = dev && phys && queue;

  O3deInteropImport import;
  const bool haveImage =
      haveDevice && O3deBackend::Instance().GetInteropImport(import);
  if (haveImage)
  {
    st.live = import.live;  // refresh each frame (static probe -> live switch)
    // Latch the per-frame timeline value the producer signalled for the latest
    // frame; PrepareForExternalSampling waits on it before sampling (#26).
    st.semaphoreWaitValue = import.semaphoreWaitValue;
  }

  if (haveImage &&
      (!st.imported || import.generation != st.importedGeneration))
  {
    if (st.imported)
    {
      // RETIRE the old import rather than freeing it now: Qt may still have an
      // in-flight frame sampling the old VkImage, and freeing it out from under
      // that frame loses Qt's device -- this was the live-resize device loss (the
      // producer recreates the image at the camera size on the first frame). Kept
      // alive until the camera is destroyed (resizes are rare). Only retire if Qt's
      // device is unchanged; if Qt recreated its device after a loss, st.ctx is
      // stale and the old VkImage died with it -- just drop the handle.
      if (st.ctx.device == dev)
        st.retiredImports.push_back(st.img);
      st.img = O3deVkImportedImage{};
      st.imported = false;
    }
    st.ctx.instance = inst;
    st.ctx.physicalDevice = phys;
    st.ctx.device = dev;
    st.ctx.queue = queue;
    st.ctx.queueFamily = FirstGraphicsFamily(phys);
    if (O3deVkImportImage(st.ctx, import, &st.img))  // consumes import.fd
    {
      st.imported = true;
      st.importedGeneration = import.generation;
      st.acquired = false;  // re-acquire layout/ownership for the new image
      gzmsg << "[gz-o3de] imported Atom image onto Qt's VkDevice (VkImage="
            << reinterpret_cast<void *>(st.img.image) << ", gen="
            << import.generation << ", " << st.img.width << "x" << st.img.height
            << ") -- native Vulkan->Vulkan display" << std::endl;
    }
    else
    {
      if (import.fd >= 0)
        ::close(import.fd);  // import failed: we still own the dup'd FD
      gzerr << "[gz-o3de] failed to import Atom image onto Qt's VkDevice"
            << std::endl;
    }
  }
  else if (haveImage)
  {
    // Generation unchanged: close the FD GetInteropImport() dup'd but we did
    // not hand to a successful import (avoids leaking one FD per frame).
    if (import.fd >= 0)
      ::close(import.fd);
  }
  else if (st.warnCount++ < 3)
  {
    gzwarn << "[gz-o3de] RenderTextureMetalId: "
           << (!haveDevice ? "Qt did not inject a Vulkan device (is the "
                             "Vulkan GUI backend active?)"
                           : "backend has no exportable image yet "
                             "(interop not ready)")
           << " -- will retry" << std::endl;
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
  if (st.live)
  {
    // Stage B: the producer renders the live scene into the image every frame.
    // Its render-finished fence-signal scope copy-reads the image last, so the
    // producer leaves it in TRANSFER_SRC_OPTIMAL (Atom maps a copy-read to that
    // layout). Acquire ownership + transition to shader-read EACH frame, waiting
    // on the producer's render-finished TIMELINE semaphore at this frame's value
    // (#26) so the cross-device write is visible before we read -- the producer
    // also host-syncs, but the semaphore wait is what makes the writes visible to
    // Qt's separate device (host-sync alone is spec-insufficient). When no
    // semaphore is advertised (GZ_O3DE_INTEROP_SEM off) the wait is a no-op.
    // The layout barrier is a no-op SHADER_READ -> SHADER_READ (old == new),
    // matching the proven static-probe acquire.
    //
    // VERIFIED working: reading this imported image back on Qt's own VkDevice
    // yields pixels identical to the producer's pipeline output (the demo
    // box/sphere/cylinder), and the live demo displays correctly. So the
    // cross-device COLOUR-ATTACHMENT render-target handoff is fine -- there is NO
    // compression/visibility problem here. (An earlier comment claimed one; it was
    // wrong. The live device loss it referred to was the resize re-import
    // use-after-free, now fixed by retiring old imports -- see
    // o3de/docs/zero-copy-interop-findings.md.)
    O3deVkAcquireFromProducer(st.ctx, st.img,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        st.semaphoreWaitValue);

    // Diagnostic (GZ_O3DE_DUMP_PNG): once the pipeline has settled, read the
    // imported image back on Qt's own device and dump it to a PPM. This is the
    // supported way to inspect what the engine renders without screen-grabbing
    // the display. Fires once (the camera is static); convert with e.g.
    // `convert /tmp/o3de_consumer.ppm out.png`.
    if (std::getenv("GZ_O3DE_DUMP_PNG"))
    {
      static int dumpFrame = 0;
      if (dumpFrame++ == 30)
      {
        std::vector<uint8_t> rgba;
        if (O3deVkReadbackImageRgba(st.ctx, st.img,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &rgba) &&
            !rgba.empty())
        {
          const char *path = std::getenv("GZ_O3DE_DUMP_PATH");
          if (!path)
            path = "/tmp/o3de_consumer.ppm";
          if (FILE *f = std::fopen(path, "wb"))
          {
            std::fprintf(f, "P6\n%u %u\n255\n", st.img.width, st.img.height);
            const size_t n = rgba.size() / 4u;
            for (size_t i = 0; i < n; ++i)
              std::fwrite(rgba.data() + i * 4u, 1u, 3u, f);  // RGB, drop alpha
            std::fclose(f);
            gzmsg << "[gz-o3de] dumped consumer image to " << path << " ("
                  << st.img.width << "x" << st.img.height << ")" << std::endl;
          }
        }
      }
    }
  }
  else if (!st.acquired)
  {
    // Static probe image (uploaded once): acquire ownership and settle the layout
    // a single time from the shader-read layout UpdateImageContents leaves.
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
