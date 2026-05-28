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
#ifndef GZ_RENDERING_O3DE_O3DEVKINTEROP_HH_
#define GZ_RENDERING_O3DE_O3DEVKINTEROP_HH_

// M4 "Strategy 2" consumer: import the backend's exported Vulkan image into a
// SECOND, independent Vulkan device -- the stand-in for Qt's QRhi VkDevice in
// gz-gui's MinimalSceneRhiVulkan path. Compiled as a normal gz-rendering TU
// (NOT the Atom TU): it includes <vulkan/vulkan.h> and must never see
// Atom/AzCore. It talks to the backend only through the plain-C++ O3deBackend
// interface. Kept behind GZ_O3DE_INTEROP so non-interop builds neither compile
// nor link the Vulkan loader.
//
// This is the Vulkan->Vulkan sibling of O3deGlInterop.cc: where the GL self-test
// imports the FD via GL_EXT_memory_object_fd, this imports it via
// VK_KHR_external_memory_fd (VkImportMemoryFdInfoKHR) and, when the producer
// exports one, a render-finished VkSemaphore via VK_KHR_external_semaphore_fd
// (VkImportSemaphoreFdInfoKHR). In production this importer lives on Qt's
// VkDevice and the bound VkImage is handed to QSGVulkanTexture::fromNative().
namespace gz
{
  namespace rendering
  {
    /// \brief Headless self-test of the Vulkan->Vulkan zero-copy import path.
    ///
    /// Fetches the exportable interop image (O3deBackend::GetInteropImport),
    /// creates a private VkInstance+VkDevice on the same physical GPU, imports
    /// the memory FD (VK_KHR_external_memory_fd) and binds a VkImage aliasing it,
    /// imports the render-finished semaphore if the producer exported one
    /// (VK_KHR_external_semaphore_fd), copies the image to a host-visible buffer
    /// and compares the texels to the deterministic gradient the backend
    /// uploaded. Logs a PASS/FAIL summary. Self-contained: it creates and tears
    /// down its own Vulkan device, so it may be run on any thread.
    /// \return True if the imported image's texels match the uploaded gradient.
    bool RunO3deInteropVkSelfTest();
  }
}
#endif
