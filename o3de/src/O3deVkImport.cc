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

// Reusable Vulkan importer (see O3deVkImport.hh). Ordinary gz-rendering TU; no
// Atom/AzCore. Compiled only in interop builds.

#if defined(GZ_O3DE_INTEROP_BUILD)

#include "O3deVkImport.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace gz
{
namespace rendering
{

namespace
{
  bool VkOk(VkResult _res, const char *_where)
  {
    if (_res == VK_SUCCESS)
      return true;
    std::fprintf(stderr, "[gz-o3de] vkimport: %s failed (VkResult=%d)\n", _where,
        static_cast<int>(_res));
    return false;
  }

  int FindMemoryType(VkPhysicalDevice _phys, uint32_t _typeBits,
      VkMemoryPropertyFlags _props)
  {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(_phys, &mp);
    for (uint32_t i = 0u; i < mp.memoryTypeCount; ++i)
    {
      if ((_typeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & _props) == _props)
        return static_cast<int>(i);
    }
    return -1;
  }
}  // namespace

//////////////////////////////////////////////////
bool O3deVkImportImage(const O3deVkDeviceContext &_ctx,
    const O3deInteropImport &_import, O3deVkImportedImage *_out)
{
  if (!_out || _ctx.device == VK_NULL_HANDLE || _import.fd < 0)
    return false;
  *_out = O3deVkImportedImage{};
  _out->width = _import.width;
  _out->height = _import.height;

  auto getFdProps = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      vkGetDeviceProcAddr(_ctx.device, "vkGetMemoryFdPropertiesKHR"));
  auto importSemFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
      vkGetDeviceProcAddr(_ctx.device, "vkImportSemaphoreFdKHR"));

  // VkImage matching the producer's create info exactly (O3deBackend.cc
  // ProveFdExportOnce). Opaque-FD sharing requires identical creation params.
  VkExternalMemoryImageCreateInfo extImg{};
  extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkImageCreateInfo imgInfo{};
  imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.pNext = &extImg;
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent = {_import.width, _import.height, 1u};
  imgInfo.mipLevels = 1u;
  imgInfo.arrayLayers = 1u;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!VkOk(vkCreateImage(_ctx.device, &imgInfo, nullptr, &_out->image),
          "vkCreateImage"))
    return false;

  VkMemoryRequirements memReq{};
  vkGetImageMemoryRequirements(_ctx.device, _out->image, &memReq);
  uint32_t typeBits = memReq.memoryTypeBits;
  if (getFdProps)
  {
    VkMemoryFdPropertiesKHR fdProps{};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (getFdProps(_ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
            _import.fd, &fdProps) == VK_SUCCESS)
      typeBits &= fdProps.memoryTypeBits;
  }
  const int memType =
      FindMemoryType(_ctx.physicalDevice, typeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memType < 0)
  {
    std::fprintf(stderr, "[gz-o3de] vkimport: no device-local memory type\n");
    O3deVkDestroyImported(_ctx, _out);
    return false;
  }

  // VkImportMemoryFdInfoKHR transfers ownership of _import.fd to the device.
  VkImportMemoryFdInfoKHR importFd{};
  importFd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  importFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  importFd.fd = _import.fd;
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &importFd;
  allocInfo.allocationSize = _import.allocationSize;  // whole VMA block
  allocInfo.memoryTypeIndex = static_cast<uint32_t>(memType);
  if (!VkOk(vkAllocateMemory(_ctx.device, &allocInfo, nullptr, &_out->memory),
          "vkAllocateMemory(import fd)"))
  {
    O3deVkDestroyImported(_ctx, _out);
    return false;
  }
  if (!VkOk(vkBindImageMemory(_ctx.device, _out->image, _out->memory,
              _import.allocationOffset), "vkBindImageMemory"))
  {
    O3deVkDestroyImported(_ctx, _out);
    return false;
  }

  // Import the render-finished semaphore if the producer exported one. The
  // producer's fence is a TIMELINE semaphore (Atom's TimelineSemaphoreFence), so
  // the consumer-side handle MUST also be created as VK_SEMAPHORE_TYPE_TIMELINE --
  // the imported OPAQUE_FD payload is a timeline, and a binary handle cannot wait
  // on it. The import is PERMANENT (flags = 0): the shared timeline payload must
  // persist so we can wait on the producer's strictly increasing per-frame values
  // (TEMPORARY import reverts after a single wait, breaking subsequent frames).
  if (_import.semaphoreFd >= 0 && importSemFd)
  {
    VkSemaphoreTypeCreateInfo semType{};
    semType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semType.initialValue = 0u;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &semType;
    if (VkOk(vkCreateSemaphore(_ctx.device, &semInfo, nullptr, &_out->semaphore),
            "vkCreateSemaphore"))
    {
      VkImportSemaphoreFdInfoKHR impSem{};
      impSem.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
      impSem.semaphore = _out->semaphore;
      impSem.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      impSem.fd = _import.semaphoreFd;
      impSem.flags = 0;
      if (!VkOk(importSemFd(_ctx.device, &impSem), "vkImportSemaphoreFdKHR"))
      {
        vkDestroySemaphore(_ctx.device, _out->semaphore, nullptr);
        _out->semaphore = VK_NULL_HANDLE;
        // The semaphore FD was not consumed; the producer/caller still owns it.
      }
    }
  }
  return true;
}

//////////////////////////////////////////////////
bool O3deVkAcquireFromProducer(const O3deVkDeviceContext &_ctx,
    const O3deVkImportedImage &_img,
    VkImageLayout _producerLayout, VkImageLayout _targetLayout,
    uint64_t _waitValue)
{
  if (_ctx.device == VK_NULL_HANDLE || _img.image == VK_NULL_HANDLE)
    return false;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = _ctx.queueFamily;
  if (!VkOk(vkCreateCommandPool(_ctx.device, &poolInfo, nullptr, &pool),
          "vkCreateCommandPool"))
    return false;

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo cbAlloc{};
  cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAlloc.commandPool = pool;
  cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAlloc.commandBufferCount = 1u;
  bool ok = VkOk(vkAllocateCommandBuffers(_ctx.device, &cbAlloc, &cmd),
      "vkAllocateCommandBuffers");

  if (ok)
  {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // DIAGNOSTIC (interop bring-up): a queue-family ownership ACQUIRE
    // (srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL) is only valid when the
    // producer issued the matching RELEASE (dst = EXTERNAL). Atom never does, so
    // this acquire is unmatched -- spec-invalid and a candidate for a delayed GPU
    // fault. With GZ_O3DE_NO_QFOT set we drop the ownership transfer and issue a
    // plain layout barrier (IGNORED -> IGNORED) to A/B test whether the unmatched
    // EXTERNAL transfer is what loses the device.
    const bool noQfot = (std::getenv("GZ_O3DE_NO_QFOT") != nullptr);
    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    acquire.oldLayout = _producerLayout;
    acquire.newLayout = _targetLayout;
    acquire.srcQueueFamilyIndex =
        noQfot ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex =
        noQfot ? VK_QUEUE_FAMILY_IGNORED : _ctx.queueFamily;
    acquire.image = _img.image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        1, &acquire);
    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(_ctx.device, &fenceInfo, nullptr, &fence);

    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    // Wait on the producer's render-finished TIMELINE semaphore at the value the
    // producer signalled for this frame (_waitValue). This is the cross-device
    // synchronisation that establishes visibility of the producer's render into
    // the shared image before this consumer queue reads it: without it the read
    // races the (separate-device) write and the GPU is lost (~5s TDR). A timeline
    // wait needs its value passed via VkTimelineSemaphoreSubmitInfo chained onto
    // the submit; a value of 0 (no semaphore advertised) skips the wait.
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount = 1u;
    timelineInfo.pWaitSemaphoreValues = &_waitValue;
    uint64_t counter = ~0ull;
    if (_img.semaphore != VK_NULL_HANDLE)
    {
      submit.pNext = &timelineInfo;
      submit.waitSemaphoreCount = 1u;
      submit.pWaitSemaphores = &_img.semaphore;
      submit.pWaitDstStageMask = &waitStage;
      // The shared timeline counter's current value. Logged below as evidence the
      // import shares the producer's payload (counter advances with the producer)
      // and that the wait cannot hang (counter >= _waitValue after host-sync). It
      // was a value mismatch / non-shared payload we needed to rule out for #26.
      auto getCounter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
          vkGetDeviceProcAddr(_ctx.device, "vkGetSemaphoreCounterValue"));
      if (getCounter)
        getCounter(_ctx.device, _img.semaphore, &counter);
    }
    const VkResult subRes = vkQueueSubmit(_ctx.queue, 1u, &submit, fence);
    ok = VkOk(subRes, "vkQueueSubmit");
    if (ok)
      vkWaitForFences(_ctx.device, 1u, &fence, VK_TRUE, UINT64_MAX);
    // Periodic acquire log (first few + every 100th) + always on a submit failure:
    // the #26 wait value vs the shared timeline counter (counter advancing in
    // lockstep is the cross-device-sync working) and the submit result.
    static int acquireCount = 0;
    if (_img.semaphore != VK_NULL_HANDLE &&
        (acquireCount++ < 3 || acquireCount % 100 == 0 || subRes != VK_SUCCESS))
      std::fprintf(stderr,
          "[gz-o3de] vkimport: acquire wait=%llu shared-counter=%llu submit=%d\n",
          static_cast<unsigned long long>(_waitValue),
          static_cast<unsigned long long>(counter), static_cast<int>(subRes));
    vkDestroyFence(_ctx.device, fence, nullptr);
  }

  vkDestroyCommandPool(_ctx.device, pool, nullptr);
  return ok;
}

//////////////////////////////////////////////////
bool O3deVkReadbackImageRgba(const O3deVkDeviceContext &_ctx,
    const O3deVkImportedImage &_img, VkImageLayout _currentLayout,
    std::vector<uint8_t> *_out)
{
  if (!_out || _ctx.device == VK_NULL_HANDLE || _img.image == VK_NULL_HANDLE ||
      _img.width == 0u || _img.height == 0u)
    return false;
  const VkDeviceSize bytes =
      static_cast<VkDeviceSize>(_img.width) * _img.height * 4u;

  // Host-visible staging buffer for the copy destination.
  VkBuffer buf = VK_NULL_HANDLE;
  VkBufferCreateInfo bufInfo{};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size = bytes;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!VkOk(vkCreateBuffer(_ctx.device, &bufInfo, nullptr, &buf),
          "vkCreateBuffer(readback)"))
    return false;

  VkMemoryRequirements memReq{};
  vkGetBufferMemoryRequirements(_ctx.device, buf, &memReq);
  const int memType = FindMemoryType(_ctx.physicalDevice, memReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memType < 0)
  {
    vkDestroyBuffer(_ctx.device, buf, nullptr);
    return false;
  }
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = static_cast<uint32_t>(memType);
  if (!VkOk(vkAllocateMemory(_ctx.device, &allocInfo, nullptr, &mem),
          "vkAllocateMemory(readback)"))
  {
    vkDestroyBuffer(_ctx.device, buf, nullptr);
    return false;
  }
  vkBindBufferMemory(_ctx.device, buf, mem, 0u);

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = _ctx.queueFamily;
  vkCreateCommandPool(_ctx.device, &poolInfo, nullptr, &pool);

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo cbAlloc{};
  cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAlloc.commandPool = pool;
  cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAlloc.commandBufferCount = 1u;
  bool ok = VkOk(vkAllocateCommandBuffers(_ctx.device, &cbAlloc, &cmd),
      "vkAllocateCommandBuffers(readback)");
  if (ok)
  {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = 0;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = _currentLayout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = _img.image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0u;
    region.bufferRowLength = 0u;
    region.bufferImageHeight = 0u;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {_img.width, _img.height, 1u};
    vkCmdCopyImageToBuffer(cmd, _img.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1u, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = _currentLayout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(_ctx.device, &fenceInfo, nullptr, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    ok = VkOk(vkQueueSubmit(_ctx.queue, 1u, &submit, fence), "vkQueueSubmit(rb)");
    if (ok)
    {
      vkWaitForFences(_ctx.device, 1u, &fence, VK_TRUE, UINT64_MAX);
      void *mapped = nullptr;
      if (vkMapMemory(_ctx.device, mem, 0u, bytes, 0, &mapped) == VK_SUCCESS)
      {
        _out->resize(static_cast<size_t>(bytes));
        std::memcpy(_out->data(), mapped, static_cast<size_t>(bytes));
        vkUnmapMemory(_ctx.device, mem);
      }
      else
        ok = false;
    }
    vkDestroyFence(_ctx.device, fence, nullptr);
  }

  vkDestroyCommandPool(_ctx.device, pool, nullptr);
  vkFreeMemory(_ctx.device, mem, nullptr);
  vkDestroyBuffer(_ctx.device, buf, nullptr);
  return ok;
}

//////////////////////////////////////////////////
void O3deVkDestroyImported(const O3deVkDeviceContext &_ctx,
    O3deVkImportedImage *_img)
{
  if (!_img || _ctx.device == VK_NULL_HANDLE)
    return;
  if (_img->image != VK_NULL_HANDLE)
    vkDestroyImage(_ctx.device, _img->image, nullptr);
  if (_img->memory != VK_NULL_HANDLE)
    vkFreeMemory(_ctx.device, _img->memory, nullptr);  // closes memory FD
  if (_img->semaphore != VK_NULL_HANDLE)
    vkDestroySemaphore(_ctx.device, _img->semaphore, nullptr);  // closes sem FD
  *_img = O3deVkImportedImage{};
}

}  // namespace rendering
}  // namespace gz

#endif  // GZ_O3DE_INTEROP_BUILD
