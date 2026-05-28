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
#ifndef GZ_RENDERING_O3DE_O3DECAMERA_HH_
#define GZ_RENDERING_O3DE_O3DECAMERA_HH_

#include <memory>

#include "gz/rendering/base/BaseCamera.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"
#include "gz/rendering/o3de/O3deSensor.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Camera class.
    ///
    /// Most camera behavior (FOV, projection, pose, image size) is handled
    /// by BaseCamera. This subclass only owns the render target that
    /// produces pixels. For the M0 stub Render() is a no-op and the pixels
    /// come from the render target's gradient Copy().
    class GZ_RENDERING_O3DE_VISIBLE O3deCamera :
      public virtual BaseCamera<O3deSensor>
    {
      /// \brief Constructor
      protected: O3deCamera();

      /// \brief Destructor
      public: virtual ~O3deCamera();

      // Documentation inherited.
      public: virtual void Render() override;

      /// \brief Native Vulkan handle for the zero-copy display path. Despite the
      /// "Metal" name (the API is shared with macOS), for the Vulkan backend
      /// this returns a VkImage -- created on Qt's injected VkDevice and aliasing
      /// Atom's exported colour image -- that gz-gui's MinimalSceneRhiVulkan
      /// hands to QSGVulkanTexture::fromNative(). No-op unless built with
      /// GZ_O3DE_INTEROP and gz-gui injected its Vulkan device.
      /// \param[out] _textureIdPtr A VkImage* to receive the handle.
      public: virtual void RenderTextureMetalId(void *_textureIdPtr) const
                  override;

      /// \brief Ensure the imported image is in a layout Qt can sample
      /// (SHADER_READ_ONLY_OPTIMAL), acquiring it from the producer and waiting
      /// on the render-finished semaphore when one is exported. Called by
      /// MinimalSceneRhiVulkan before sampling.
      public: virtual void PrepareForExternalSampling() override;

      // Documentation inherited.
      protected: virtual RenderTargetPtr RenderTarget() const override;

      // Documentation inherited.
      protected: virtual void Init() override;

      /// \brief Create the render target used for offscreen rendering
      protected: virtual void CreateRenderTexture();

      /// \brief Pointer to the render target
      protected: O3deRenderTargetPtr renderTexture;

      /// \brief Holds the VkImage imported onto Qt's device for the zero-copy
      /// path, plus its backing memory/semaphore and the device context. Opaque
      /// here so this public header carries no Vulkan types; defined in the .cc.
      /// Lazily populated on the first RenderTextureMetalId() call.
      private: class O3deCameraInterop;
      private: mutable std::unique_ptr<O3deCameraInterop> interop;

      /// \brief Make the scene our friend so it can create cameras
      private: friend class O3deScene;
    };
    }
  }
}
#endif
