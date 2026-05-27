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
#ifndef GZ_RENDERING_O3DE_O3DERENDERTARGET_HH_
#define GZ_RENDERING_O3DE_O3DERENDERTARGET_HH_

#include <gz/math/Color.hh>

#include "gz/rendering/base/BaseRenderTarget.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the RenderTarget class.
    ///
    /// This is where the CPU-readback bridge lives. For the M0 stub
    /// Copy() fills the destination image with a gradient instead of
    /// reading back an Atom-rendered offscreen buffer. A later milestone
    /// will drive an Atom frame and use RPI::AttachmentReadback here.
    class GZ_RENDERING_O3DE_VISIBLE O3deRenderTarget :
      public virtual BaseRenderTarget<O3deObject>
    {
      /// \brief Constructor
      protected: O3deRenderTarget();

      /// \brief Destructor
      public: virtual ~O3deRenderTarget();

      /// \brief Render one frame to the offscreen target. No-op in the stub.
      public: virtual void Render();

      /// \brief Copy the render target buffer data to an image.
      /// \param[in] _image Image to copy the data to (sized by the caller).
      public: virtual void Copy(Image &_image) const override;

      // Documentation inherited.
      public: virtual void Destroy() override;

      // Documentation inherited.
      protected: virtual void RebuildImpl() override;

      /// \brief Non-owning back-pointer to the camera that owns this target.
      /// Set by O3deCamera when it creates the target; used by Copy() to read
      /// the camera pose/projection and walk the scene to be rendered. The
      /// camera outlives the target it owns.
      protected: O3deCamera *camera = nullptr;

      /// \brief Make the camera our friend so it can drive the render
      private: friend class O3deCamera;

      /// \brief Make the scene our friend so it can create render targets
      private: friend class O3deScene;
    };
    }
  }
}
#endif
