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

// M4 "Strategy 2" Vulkan->Vulkan consumer SELF-TEST. Ordinary gz-rendering TU
// (no Atom/AzCore). It stands up its own private VkInstance/VkDevice (the
// stand-in for Qt's QRhi device), then imports + samples the backend's exported
// image through the SHARED importer (O3deVkImport.*) -- the exact code path
// O3deCamera uses on Qt's real device. Proves the FD round-trips bit-exact.

#include "O3deVkInterop.hh"

#if !defined(GZ_O3DE_INTEROP_BUILD)
// Patch-free / non-interop build: no Vulkan dependency, self-test is a no-op.
namespace gz { namespace rendering {
bool RunO3deInteropVkSelfTest() { return false; }
}}  // namespace gz::rendering
#else

#include "O3deBackend.hh"
#include "O3deVkImport.hh"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

#include <vulkan/vulkan.h>

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
    std::fprintf(stderr, "[gz-o3de] vktest: %s failed (VkResult=%d)\n", _where,
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

bool RunO3deInteropVkSelfTest()
{
  O3deInteropImport import;
  if (!O3deBackend::Instance().GetInteropImport(import))
  {
    std::fprintf(stderr,
        "[gz-o3de] vktest: GetInteropImport() returned false (interop not "
        "built/enabled, or image not ready) -- skipping Vulkan self-test\n");
    return false;
  }
  std::fprintf(stderr,
      "[gz-o3de] vktest: importing memFd=%d semFd=%d %ux%u allocSize=%llu "
      "offset=%llu\n",
      import.fd, import.semaphoreFd, import.width, import.height,
      static_cast<unsigned long long>(import.allocationSize),
      static_cast<unsigned long long>(import.allocationOffset));

  O3deVkDeviceContext ctx;
  VkBuffer readbackBuf = VK_NULL_HANDLE;
  VkDeviceMemory readbackMem = VK_NULL_HANDLE;
  VkCommandPool cmdPool = VK_NULL_HANDLE;
  O3deVkImportedImage imported;
  bool importOk = false;
  bool match = false;

  auto done = [&](bool _result) -> bool
  {
    if (ctx.device != VK_NULL_HANDLE)
      vkDeviceWaitIdle(ctx.device);
    if (readbackBuf != VK_NULL_HANDLE)
      vkDestroyBuffer(ctx.device, readbackBuf, nullptr);
    if (readbackMem != VK_NULL_HANDLE)
      vkFreeMemory(ctx.device, readbackMem, nullptr);
    if (cmdPool != VK_NULL_HANDLE)
      vkDestroyCommandPool(ctx.device, cmdPool, nullptr);
    O3deVkDestroyImported(ctx, &imported);  // closes the consumed FDs
    // If the import never succeeded, the device did not take ownership of the
    // FDs -- close them ourselves so the one-shot test does not leak.
    if (!importOk)
    {
      if (import.fd >= 0)
        ::close(import.fd);
      if (import.semaphoreFd >= 0)
        ::close(import.semaphoreFd);
    }
    if (ctx.device != VK_NULL_HANDLE)
      vkDestroyDevice(ctx.device, nullptr);
    if (ctx.instance != VK_NULL_HANDLE)
      vkDestroyInstance(ctx.instance, nullptr);
    std::fprintf(stderr,
        "[gz-o3de] vktest: %s -- Vulkan->Vulkan zero-copy import %s\n",
        _result ? "PASS" : "FAIL", _result ? "verified" : "did NOT verify");
    return _result;
  };

  // ---- Private device (the Qt-device stand-in). ----
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "gz-o3de-vk-interop-selftest";
  appInfo.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instInfo{};
  instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instInfo.pApplicationInfo = &appInfo;
  if (!VkOk(vkCreateInstance(&instInfo, nullptr, &ctx.instance),
          "vkCreateInstance"))
    return done(false);

  uint32_t physCount = 0u;
  vkEnumeratePhysicalDevices(ctx.instance, &physCount, nullptr);
  if (physCount == 0u)
  {
    std::fprintf(stderr, "[gz-o3de] vktest: no Vulkan physical devices\n");
    return done(false);
  }
  std::vector<VkPhysicalDevice> physs(physCount);
  vkEnumeratePhysicalDevices(ctx.instance, &physCount, physs.data());
  ctx.physicalDevice = physs[0];
  for (VkPhysicalDevice p : physs)
  {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(p, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      ctx.physicalDevice = p;
      break;
    }
  }
  {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
    std::fprintf(stderr, "[gz-o3de] vktest: device = %s\n", props.deviceName);
  }

  uint32_t qfCount = 0u;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qfCount,
      qfs.data());
  ctx.queueFamily = UINT32_MAX;
  for (uint32_t i = 0u; i < qfCount; ++i)
  {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      ctx.queueFamily = i;
      break;
    }
  }
  if (ctx.queueFamily == UINT32_MAX)
  {
    std::fprintf(stderr, "[gz-o3de] vktest: no graphics queue family\n");
    return done(false);
  }

  uint32_t extCount = 0u;
  vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &extCount,
      nullptr);
  std::vector<VkExtensionProperties> exts(extCount);
  vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &extCount,
      exts.data());
  auto hasExt = [&](const char *_name) -> bool
  {
    for (const auto &e : exts)
      if (std::strcmp(e.extensionName, _name) == 0)
        return true;
    return false;
  };
  std::vector<const char *> enabledExts;
  for (const char *e : {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
  {
    if (hasExt(e))
      enabledExts.push_back(e);
  }
  if (!hasExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
  {
    std::fprintf(stderr,
        "[gz-o3de] vktest: device lacks VK_KHR_external_memory_fd\n");
    return done(false);
  }

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qInfo{};
  qInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qInfo.queueFamilyIndex = ctx.queueFamily;
  qInfo.queueCount = 1u;
  qInfo.pQueuePriorities = &prio;
  VkDeviceCreateInfo devInfo{};
  devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  devInfo.queueCreateInfoCount = 1u;
  devInfo.pQueueCreateInfos = &qInfo;
  devInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
  devInfo.ppEnabledExtensionNames = enabledExts.data();
  if (!VkOk(vkCreateDevice(ctx.physicalDevice, &devInfo, nullptr, &ctx.device),
          "vkCreateDevice"))
    return done(false);
  vkGetDeviceQueue(ctx.device, ctx.queueFamily, 0u, &ctx.queue);

  // ---- The bit under test: import via the SHARED helper, then acquire it for
  // reading (TRANSFER_SRC) exactly as O3deCamera acquires it for sampling. ----
  if (!O3deVkImportImage(ctx, import, &imported))
    return done(false);
  importOk = true;
  if (!O3deVkAcquireFromProducer(ctx, imported,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
    return done(false);

  // ---- Host-visible readback buffer + copy the imported image into it. ----
  const VkDeviceSize bufSize =
      static_cast<VkDeviceSize>(import.width) * import.height * 4u;
  VkBufferCreateInfo bufInfo{};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size = bufSize;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!VkOk(vkCreateBuffer(ctx.device, &bufInfo, nullptr, &readbackBuf),
          "vkCreateBuffer"))
    return done(false);
  VkMemoryRequirements bufReq{};
  vkGetBufferMemoryRequirements(ctx.device, readbackBuf, &bufReq);
  const int bufType = FindMemoryType(ctx.physicalDevice, bufReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (bufType < 0)
  {
    std::fprintf(stderr, "[gz-o3de] vktest: no host-visible memory type\n");
    return done(false);
  }
  VkMemoryAllocateInfo bufAlloc{};
  bufAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  bufAlloc.allocationSize = bufReq.size;
  bufAlloc.memoryTypeIndex = static_cast<uint32_t>(bufType);
  if (!VkOk(vkAllocateMemory(ctx.device, &bufAlloc, nullptr, &readbackMem),
          "vkAllocateMemory(readback)"))
    return done(false);
  vkBindBufferMemory(ctx.device, readbackBuf, readbackMem, 0u);

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = ctx.queueFamily;
  if (!VkOk(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &cmdPool),
          "vkCreateCommandPool"))
    return done(false);
  VkCommandBufferAllocateInfo cbAlloc{};
  cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAlloc.commandPool = cmdPool;
  cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAlloc.commandBufferCount = 1u;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!VkOk(vkAllocateCommandBuffers(ctx.device, &cbAlloc, &cmd),
          "vkAllocateCommandBuffers"))
    return done(false);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
  region.imageExtent = {import.width, import.height, 1u};
  vkCmdCopyImageToBuffer(cmd, imported.image,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuf, 1u, &region);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1u;
  submit.pCommandBuffers = &cmd;
  if (!VkOk(vkQueueSubmit(ctx.queue, 1u, &submit, VK_NULL_HANDLE),
          "vkQueueSubmit"))
    return done(false);
  vkQueueWaitIdle(ctx.queue);

  // ---- Map + compare to the uploaded gradient (R=x, G=y, B=128, A=255). ----
  void *mapped = nullptr;
  if (!VkOk(vkMapMemory(ctx.device, readbackMem, 0u, bufSize, 0, &mapped),
          "vkMapMemory"))
    return done(false);
  const auto *pixels = static_cast<const uint8_t *>(mapped);

  size_t good = 0;
  size_t total = 0;
  int firstBadX = -1, firstBadY = -1;
  for (uint32_t y = 0u; y < import.height; ++y)
  {
    for (uint32_t x = 0u; x < import.width; ++x)
    {
      const uint8_t *px = &pixels[(static_cast<size_t>(y) * import.width + x) * 4u];
      const bool pixelOk = px[0] == static_cast<uint8_t>(x) &&
          px[1] == static_cast<uint8_t>(y) && px[2] == 128u && px[3] == 255u;
      if (pixelOk)
        ++good;
      else if (firstBadX < 0)
      {
        firstBadX = static_cast<int>(x);
        firstBadY = static_cast<int>(y);
      }
      ++total;
    }
  }
  match = (good == total);
  const uint8_t *s = &pixels[0];
  const uint8_t *m = &pixels[(static_cast<size_t>(import.height / 2u) *
      import.width + import.width / 2u) * 4u];
  std::fprintf(stderr,
      "[gz-o3de] vktest: %zu/%zu texels matched the gradient. "
      "sample(0,0)=[%u %u %u %u] expect[0 0 128 255]; center=[%u %u %u %u]\n",
      good, total, s[0], s[1], s[2], s[3], m[0], m[1], m[2], m[3]);
  if (!match)
    std::fprintf(stderr,
        "[gz-o3de] vktest: first mismatch at (%d,%d)\n", firstBadX, firstBadY);

  // ---- Sampler-path readback (stabilises the "cross-device sampler works"
  // finding). Reads exactly what Qt's QSGSimpleTextureNode draw samples --
  // texelFetch through a VkSampler on the consumer device -- and asserts it is
  // byte-identical to the transfer-copy readback above. If the producer image,
  // memory import or sampler swizzle were broken on this consumer device, this
  // would diverge from the transfer copy. The reusable helper is
  // O3deVkSampleProbeRgba (O3deVkImport.hh), also exposed as the runtime
  // GZ_O3DE_SAMPLE_PROBE diagnostic (see o3de/docs/diagnostic-tools.md). The
  // imported image is currently in TRANSFER_SRC_OPTIMAL (the layout the
  // transfer readback left it in); the probe transitions it through
  // SHADER_READ_ONLY_OPTIMAL and restores it.
  bool samplerMatch = false;
  {
    std::vector<uint8_t> probeRgba;
    if (!O3deVkSampleProbeRgba(ctx, imported,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &probeRgba))
    {
      std::fprintf(stderr,
          "[gz-o3de] vksamplertest: O3deVkSampleProbeRgba failed -- "
          "cross-device sampler readback not verified\n");
    }
    else if (probeRgba.size() != static_cast<size_t>(bufSize))
    {
      std::fprintf(stderr,
          "[gz-o3de] vksamplertest: probe size mismatch: probe=%zu transfer=%zu\n",
          probeRgba.size(), static_cast<size_t>(bufSize));
    }
    else
    {
      size_t sampGood = 0;
      int sampBadX = -1, sampBadY = -1;
      for (uint32_t y = 0u; y < import.height; ++y)
      {
        for (uint32_t x = 0u; x < import.width; ++x)
        {
          const size_t off = (static_cast<size_t>(y) * import.width + x) * 4u;
          if (probeRgba[off + 0u] == pixels[off + 0u] &&
              probeRgba[off + 1u] == pixels[off + 1u] &&
              probeRgba[off + 2u] == pixels[off + 2u] &&
              probeRgba[off + 3u] == pixels[off + 3u])
          {
            ++sampGood;
          }
          else if (sampBadX < 0)
          {
            sampBadX = static_cast<int>(x);
            sampBadY = static_cast<int>(y);
          }
        }
      }
      samplerMatch = (sampGood == total);
      const uint8_t *ps = probeRgba.data();
      const size_t cOff = (static_cast<size_t>(import.height / 2u) *
          import.width + import.width / 2u) * 4u;
      std::fprintf(stderr,
          "[gz-o3de] vksamplertest: %zu/%zu texels match the transfer copy. "
          "probe(0,0)=[%u %u %u %u] transfer(0,0)=[%u %u %u %u]; "
          "probe.center=[%u %u %u %u]\n",
          sampGood, total, ps[0], ps[1], ps[2], ps[3], s[0], s[1], s[2], s[3],
          ps[cOff + 0u], ps[cOff + 1u], ps[cOff + 2u], ps[cOff + 3u]);
      if (!samplerMatch)
        std::fprintf(stderr,
            "[gz-o3de] vksamplertest: first sampler/transfer mismatch at "
            "(%d,%d)\n", sampBadX, sampBadY);
      std::fprintf(stderr,
          "[gz-o3de] vksamplertest: %s -- cross-device texture sampler %s\n",
          samplerMatch ? "PASS" : "FAIL",
          samplerMatch ? "verified" : "did NOT verify");
    }
  }

  if (const char *dumpPath = std::getenv("GZ_O3DE_INTEROP_VKDUMP"))
  {
    if (FILE *fp = std::fopen(dumpPath, "wb"))
    {
      std::fprintf(fp, "P6\n%u %u\n255\n", import.width, import.height);
      std::vector<uint8_t> rgb(static_cast<size_t>(import.width) *
          import.height * 3u);
      for (size_t i = 0, n = static_cast<size_t>(import.width) * import.height;
          i < n; ++i)
      {
        rgb[i * 3u + 0u] = pixels[i * 4u + 0u];
        rgb[i * 3u + 1u] = pixels[i * 4u + 1u];
        rgb[i * 3u + 2u] = pixels[i * 4u + 2u];
      }
      std::fwrite(rgb.data(), 1u, rgb.size(), fp);
      std::fclose(fp);
      std::fprintf(stderr,
          "[gz-o3de] vktest: wrote imported image to %s\n", dumpPath);
    }
  }

  vkUnmapMemory(ctx.device, readbackMem);
  // Overall PASS requires both: transfer-copy texels matched the producer's
  // uploaded gradient (existing "vktest"), AND sampler-path readback was
  // byte-identical to the transfer-copy (new "vksamplertest"). The sampler
  // half stabilises the empirical finding that cross-device sampling of an
  // OPAQUE_FD-imported OPTIMAL image on NVIDIA proprietary works correctly
  // when the producer uses a dedicated allocation -- a regression in either
  // the producer's export or the consumer's import would surface here.
  return done(match && samplerMatch);
}

}  // namespace rendering
}  // namespace gz

#endif  // GZ_O3DE_INTEROP_BUILD
