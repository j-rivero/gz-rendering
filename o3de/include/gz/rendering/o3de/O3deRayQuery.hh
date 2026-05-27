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
#ifndef GZ_RENDERING_O3DE_O3DERAYQUERY_HH_
#define GZ_RENDERING_O3DE_O3DERAYQUERY_HH_

#include "gz/rendering/base/BaseRayQuery.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the RayQuery class.
    ///
    /// M0 stub: BaseRayQuery provides a fully functional ray definition and
    /// a default (no-intersection) ClosestPoint(), which is enough to keep
    /// gz-gui's mouse-picking code from dereferencing a null query.
    class GZ_RENDERING_O3DE_VISIBLE O3deRayQuery :
      public BaseRayQuery<O3deObject>
    {
      /// \brief Constructor
      protected: O3deRayQuery();

      /// \brief Destructor
      public: virtual ~O3deRayQuery();

      // Keep the base CameraPtr overload visible alongside the
      // WideAngleCameraPtr override below.
      public: using BaseRayQuery<O3deObject>::SetFromCamera;

      // Documentation inherited.
      public: virtual void SetFromCamera(const WideAngleCameraPtr &_camera,
                  uint32_t _faceIdx, const math::Vector2d &_coord) override;

      /// \brief Make the scene our friend so it can create ray queries
      private: friend class O3deScene;
    };
    }
  }
}
#endif
