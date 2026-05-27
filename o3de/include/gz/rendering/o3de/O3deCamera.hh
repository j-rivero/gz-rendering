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

      // Documentation inherited.
      protected: virtual RenderTargetPtr RenderTarget() const override;

      // Documentation inherited.
      protected: virtual void Init() override;

      /// \brief Create the render target used for offscreen rendering
      protected: virtual void CreateRenderTexture();

      /// \brief Pointer to the render target
      protected: O3deRenderTargetPtr renderTexture;

      /// \brief Make the scene our friend so it can create cameras
      private: friend class O3deScene;
    };
    }
  }
}
#endif
