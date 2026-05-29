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
#ifndef GZ_RENDERING_O3DE_O3DEVKIMPORT_HH_
#define GZ_RENDERING_O3DE_O3DEVKIMPORT_HH_

// Reusable Vulkan importer for the M4 zero-copy path. Given a CALLER-PROVIDED
// Vulkan device, it aliases the backend's exported colour image
// (O3deBackend::GetInteropImport) as a VkImage on that device via
// VK_KHR_external_memory_fd, optionally importing a render-finished semaphore via
// VK_KHR_external_semaphore_fd. The same code serves two callers:
//   * O3deVkInterop.cc -- a headless self-test, on its own private VkDevice.
//   * O3deCamera       -- on Qt's QRhi VkDevice, returning the VkImage that
//                         MinimalSceneRhiVulkan hands to QSGVulkanTexture.
//
// Interop-only (gated by GZ_O3DE_INTEROP_BUILD by the includer); this header
// exposes Vulkan types, so include it only from interop builds.
#include "O3deBackend.hh"

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace gz
{
  namespace rendering
  {
    /// \brief A Vulkan device + queue the importer creates objects on. For the
    /// native path these are Qt's QRhi handles; for the self-test, a private
    /// device. \ref instance is unused by the import itself but kept for symmetry.
    struct O3deVkDeviceContext
    {
      VkInstance instance = VK_NULL_HANDLE;
      VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
      VkDevice device = VK_NULL_HANDLE;
      VkQueue queue = VK_NULL_HANDLE;
      uint32_t queueFamily = 0u;
    };

    /// \brief Handles produced by importing the exported image into a device.
    struct O3deVkImportedImage
    {
      VkImage image = VK_NULL_HANDLE;       //!< Aliases the producer's memory.
      VkDeviceMemory memory = VK_NULL_HANDLE; //!< The imported OPAQUE_FD block.
      VkSemaphore semaphore = VK_NULL_HANDLE; //!< Imported render-finished, or null.
      uint32_t width = 0u;
      uint32_t height = 0u;
    };

    /// \brief Create a VkImage on \p _ctx aliasing the exported FD memory, bind
    /// it at the reported allocation offset, and import the render-finished
    /// semaphore if \p _import carries one. The VkImage is created with the
    /// producer's exact parameters (R8G8B8A8_UNORM, OPTIMAL,
    /// COLOR|SAMPLED|TRANSFER_SRC|TRANSFER_DST) -- opaque-FD sharing requires
    /// matching creation info. The image is left in VK_IMAGE_LAYOUT_UNDEFINED;
    /// call O3deVkAcquireFromProducer() before sampling. On success the FDs in
    /// \p _import are consumed (the device owns them and closes them on free).
    /// \return True on success; on failure nothing is left allocated.
    bool O3deVkImportImage(const O3deVkDeviceContext &_ctx,
        const O3deInteropImport &_import, O3deVkImportedImage *_out);

    /// \brief Acquire \p _img from the producer (queue-family ownership transfer
    /// from VK_QUEUE_FAMILY_EXTERNAL) and transition it from \p _producerLayout
    /// to \p _targetLayout, waiting on its imported render-finished TIMELINE
    /// semaphore at value \p _waitValue if a semaphore was imported. Submits on
    /// \p _ctx.queue and blocks on a fence. \p _producerLayout is the layout the
    /// producer left the image in. \p _waitValue is the per-frame timeline value
    /// the producer signalled (from O3deInteropImport::semaphoreWaitValue); pass 0
    /// when no semaphore is in play. \return True on success.
    bool O3deVkAcquireFromProducer(const O3deVkDeviceContext &_ctx,
        const O3deVkImportedImage &_img,
        VkImageLayout _producerLayout, VkImageLayout _targetLayout,
        uint64_t _waitValue = 0u);

    /// \brief Destroy the image, free the imported memory (closing the memory
    /// FD) and destroy the semaphore (closing its FD). Safe on partial handles.
    void O3deVkDestroyImported(const O3deVkDeviceContext &_ctx,
        O3deVkImportedImage *_img);

    /// \brief Diagnostic: copy the imported image (as it appears on \p _ctx's
    /// device) into a CPU RGBA8 buffer. Lets callers verify what the consumer's
    /// device actually sees in the shared memory without screen-grabbing the
    /// display (e.g. dump to a PPM under GZ_O3DE_DUMP_PNG). Transitions \p _img
    /// from \p _currentLayout to TRANSFER_SRC and back, vkCmdCopyImageToBuffer
    /// into a host-visible staging buffer, and blocks on a fence.
    /// \return True on success, with \p _out filled (width*height*4 bytes,
    /// R8G8B8A8 order).
    bool O3deVkReadbackImageRgba(const O3deVkDeviceContext &_ctx,
        const O3deVkImportedImage &_img, VkImageLayout _currentLayout,
        std::vector<uint8_t> *_out);
  }
}
#endif
