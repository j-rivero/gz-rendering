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
#ifndef GZ_RENDERING_O3DE_O3DEFRUSTUMVISUAL_HH_
#define GZ_RENDERING_O3DE_O3DEFRUSTUMVISUAL_HH_

#include "gz/rendering/base/BaseFrustumVisual.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of a frustum visual.
    ///
    /// Inherits all frustum state (near/far clip, hfov, aspect ratio) from
    /// BaseFrustumVisual. Rendering happens in the gather/backend: when
    /// O3deRenderTarget::GatherFrame walks the scene's visuals it
    /// dynamic_pointer_casts to FrustumVisual, emits an O3deShapeData of type
    /// FRUSTUM with those four scalars + the visual's world pose, and
    /// O3deBackend::SubmitPrimitives draws the 12 edges + 4 apex-to-near
    /// connectors as AuxGeom DrawLines.
    class GZ_RENDERING_O3DE_VISIBLE O3deFrustumVisual :
      public BaseFrustumVisual<O3deVisual>
    {
      /// \brief Constructor
      protected: O3deFrustumVisual();

      /// \brief Destructor
      public: virtual ~O3deFrustumVisual();

      /// \brief Only the scene can instantiate a frustum visual
      private: friend class O3deScene;
    };
    }
  }
}
#endif
