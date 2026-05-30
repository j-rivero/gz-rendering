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
#include "O3deSampleProbeSpv.h"

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
  // STORAGE matches the producer's ShaderWrite bind flag (O3deBackend.cc
  // EnsureInteropImage): it disables NVIDIA DCC compression so the shared colour
  // plane is sampler-correct on this device. Opaque-FD sharing requires the
  // VkImageCreateInfo (incl. usage) to be identical on both sides.
  imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
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
  // The producer allocates the exported image as a DEDICATED allocation (one
  // VkDeviceMemory per image, bound at offset 0, carrying
  // VkMemoryDedicatedAllocateInfo -- see Atom RHI Image::Init). An OPAQUE_FD
  // import of dedicated memory must mirror that: chain a
  // VkMemoryDedicatedAllocateInfo referencing the image we bind here, and bind
  // at offset 0. Required on NVIDIA proprietary for the importing device's
  // sampler to interpret the shared image's block-linear tiling correctly; a
  // sub-allocated (non-dedicated, non-zero-offset) external image samples as a
  // constant even though transfer/copy reads the correct pixels.
  VkImportMemoryFdInfoKHR importFd{};
  importFd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  importFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  importFd.fd = _import.fd;
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = _out->image;
  dedicated.pNext = &importFd;
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &dedicated;
  allocInfo.allocationSize = _import.allocationSize;  // dedicated allocation size
  allocInfo.memoryTypeIndex = static_cast<uint32_t>(memType);
  if (!VkOk(vkAllocateMemory(_ctx.device, &allocInfo, nullptr, &_out->memory),
          "vkAllocateMemory(import fd)"))
  {
    O3deVkDestroyImported(_ctx, _out);
    return false;
  }
  // Dedicated memory binds at offset 0 (the producer's image owns the whole
  // allocation); the spec requires memoryOffset == 0 for dedicated memory.
  if (!VkOk(vkBindImageMemory(_ctx.device, _out->image, _out->memory,
              0u), "vkBindImageMemory"))
  {
    O3deVkDestroyImported(_ctx, _out);
    return false;
  }
  std::fprintf(stderr,
      "[gz-o3de] vkimport: DEDICATED import OK (size=%llu offset=0, "
      "VkMemoryDedicatedAllocateInfo image=%p) %ux%u\n",
      static_cast<unsigned long long>(_import.allocationSize),
      reinterpret_cast<void *>(_out->image), _out->width, _out->height);

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
bool O3deVkClearImageDiag(const O3deVkDeviceContext &_ctx,
    const O3deVkImportedImage &_img, VkImageLayout _currentLayout,
    float _r, float _g, float _b, float _a)
{
  if (_ctx.device == VK_NULL_HANDLE || _img.image == VK_NULL_HANDLE)
    return false;

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
      "vkAllocateCommandBuffers(clear)");
  if (ok)
  {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = _currentLayout;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = _img.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkClearColorValue color{};
    color.float32[0] = _r; color.float32[1] = _g;
    color.float32[2] = _b; color.float32[3] = _a;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdClearColorImage(cmd, _img.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u, &range);

    VkImageMemoryBarrier back = toDst;
    back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
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
    ok = VkOk(vkQueueSubmit(_ctx.queue, 1u, &submit, fence),
        "vkQueueSubmit(clear)");
    if (ok)
      vkWaitForFences(_ctx.device, 1u, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(_ctx.device, fence, nullptr);
  }

  vkDestroyCommandPool(_ctx.device, pool, nullptr);
  return ok;
}

//////////////////////////////////////////////////
bool O3deVkSampleProbeRgba(const O3deVkDeviceContext &_ctx,
    const O3deVkImportedImage &_img, VkImageLayout _currentLayout,
    std::vector<uint8_t> *_out)
{
  if (!_out || _ctx.device == VK_NULL_HANDLE || _img.image == VK_NULL_HANDLE ||
      _img.width == 0u || _img.height == 0u)
    return false;
  const VkDeviceSize bytes =
      static_cast<VkDeviceSize>(_img.width) * _img.height * 4u;
  const VkDevice dev = _ctx.device;
  bool ok = true;

  // Sampled image view + nearest sampler over the imported image.
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  VkCommandPool cpool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = _img.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  ok = ok && VkOk(vkCreateImageView(dev, &viewInfo, nullptr, &view),
      "vkCreateImageView(probe)");

  VkSamplerCreateInfo sampInfo{};
  sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampInfo.magFilter = VK_FILTER_NEAREST;
  sampInfo.minFilter = VK_FILTER_NEAREST;
  sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ok = ok && VkOk(vkCreateSampler(dev, &sampInfo, nullptr, &sampler),
      "vkCreateSampler(probe)");

  // Host-visible storage buffer for the compute output (packed RGBA8).
  VkBufferCreateInfo bufInfo{};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size = bytes;
  bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ok = ok && VkOk(vkCreateBuffer(dev, &bufInfo, nullptr, &buf),
      "vkCreateBuffer(probe)");
  if (ok)
  {
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    const int mt = FindMemoryType(_ctx.physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0)
      ok = false;
    else
    {
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = static_cast<uint32_t>(mt);
      ok = VkOk(vkAllocateMemory(dev, &ai, nullptr, &mem),
          "vkAllocateMemory(probe)");
      if (ok)
        vkBindBufferMemory(dev, buf, mem, 0u);
    }
  }

  // Descriptor set layout: 0=combined image sampler, 1=storage buffer.
  if (ok)
  {
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0u;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1u;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1u;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1u;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2u;
    dslInfo.pBindings = b;
    ok = VkOk(vkCreateDescriptorSetLayout(dev, &dslInfo, nullptr, &dsl),
        "vkCreateDescriptorSetLayout(probe)");
  }
  if (ok)
  {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0u;
    pc.size = 2u * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1u;
    plInfo.pSetLayouts = &dsl;
    plInfo.pushConstantRangeCount = 1u;
    plInfo.pPushConstantRanges = &pc;
    ok = VkOk(vkCreatePipelineLayout(dev, &plInfo, nullptr, &pl),
        "vkCreatePipelineLayout(probe)");
  }
  if (ok)
  {
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = sizeof(kO3deSampleProbeSpv);
    smInfo.pCode = kO3deSampleProbeSpv;
    ok = VkOk(vkCreateShaderModule(dev, &smInfo, nullptr, &shader),
        "vkCreateShaderModule(probe)");
  }
  if (ok)
  {
    VkComputePipelineCreateInfo cpInfo{};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = shader;
    cpInfo.stage.pName = "main";
    cpInfo.layout = pl;
    ok = VkOk(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1u, &cpInfo, nullptr,
        &pipe), "vkCreateComputePipelines(probe)");
  }
  VkDescriptorSet dset = VK_NULL_HANDLE;
  if (ok)
  {
    VkDescriptorPoolSize ps[2]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[0].descriptorCount = 1u;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[1].descriptorCount = 1u;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = 1u;
    dpInfo.poolSizeCount = 2u;
    dpInfo.pPoolSizes = ps;
    ok = VkOk(vkCreateDescriptorPool(dev, &dpInfo, nullptr, &dpool),
        "vkCreateDescriptorPool(probe)");
  }
  if (ok)
  {
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = dpool;
    dsAlloc.descriptorSetCount = 1u;
    dsAlloc.pSetLayouts = &dsl;
    ok = VkOk(vkAllocateDescriptorSets(dev, &dsAlloc, &dset),
        "vkAllocateDescriptorSets(probe)");
  }
  if (ok)
  {
    VkDescriptorImageInfo dii{};
    dii.sampler = sampler;
    dii.imageView = view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo dbi{};
    dbi.buffer = buf;
    dbi.offset = 0u;
    dbi.range = bytes;
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = dset;
    w[0].dstBinding = 0u;
    w[0].descriptorCount = 1u;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &dii;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = dset;
    w[1].dstBinding = 1u;
    w[1].descriptorCount = 1u;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 2u, w, 0u, nullptr);
  }

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = _ctx.queueFamily;
  if (ok)
    vkCreateCommandPool(dev, &poolInfo, nullptr, &cpool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (ok)
  {
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = cpool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1u;
    ok = VkOk(vkAllocateCommandBuffers(dev, &cb, &cmd),
        "vkAllocateCommandBuffers(probe)");
  }
  if (ok)
  {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Image -> SHADER_READ for the compute sampler read (matches Qt's sampling).
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = 0;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = _currentLayout;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = _img.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toRead);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0u, 1u,
        &dset, 0u, nullptr);
    const uint32_t pcData[2] = {_img.width, _img.height};
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
        2u * sizeof(uint32_t), pcData);
    vkCmdDispatch(cmd, (_img.width + 7u) / 8u, (_img.height + 7u) / 8u, 1u);

    VkBufferMemoryBarrier bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf;
    bb.offset = 0u;
    bb.size = bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);

    // Restore the image to the layout the caller expects.
    VkImageMemoryBarrier back = toRead;
    back.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.newLayout = _currentLayout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &fi, nullptr, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    ok = VkOk(vkQueueSubmit(_ctx.queue, 1u, &submit, fence),
        "vkQueueSubmit(probe)");
    if (ok)
    {
      vkWaitForFences(dev, 1u, &fence, VK_TRUE, UINT64_MAX);
      void *mapped = nullptr;
      if (vkMapMemory(dev, mem, 0u, bytes, 0, &mapped) == VK_SUCCESS)
      {
        _out->resize(static_cast<size_t>(bytes));
        std::memcpy(_out->data(), mapped, static_cast<size_t>(bytes));
        vkUnmapMemory(dev, mem);
      }
      else
        ok = false;
    }
  }

  if (fence) vkDestroyFence(dev, fence, nullptr);
  if (cpool) vkDestroyCommandPool(dev, cpool, nullptr);
  if (dpool) vkDestroyDescriptorPool(dev, dpool, nullptr);
  if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
  if (shader) vkDestroyShaderModule(dev, shader, nullptr);
  if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
  if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
  if (mem) vkFreeMemory(dev, mem, nullptr);
  if (buf) vkDestroyBuffer(dev, buf, nullptr);
  if (sampler) vkDestroySampler(dev, sampler, nullptr);
  if (view) vkDestroyImageView(dev, view, nullptr);
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
