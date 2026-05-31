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
#ifndef GZ_RENDERING_O3DE_O3DECAPSULE_HH_
#define GZ_RENDERING_O3DE_O3DECAPSULE_HH_

#include "gz/rendering/base/BaseCapsule.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of a Capsule geometry.
    ///
    /// Stores the capsule parameters (radius, length) via BaseCapsule; the
    /// render backend reads them when gathering the frame and emits a cylinder
    /// + 2 spheres as AuxGeom (see O3deBackend SubmitPrimitives, Type::CAPSULE).
    class GZ_RENDERING_O3DE_VISIBLE O3deCapsule :
      public BaseCapsule<O3deGeometry>
    {
      /// \brief Constructor
      protected: O3deCapsule();

      /// \brief Destructor
      public: virtual ~O3deCapsule();

      /// \brief Make the scene our friend so it can create capsules
      private: friend class O3deScene;
    };
    }
  }
}
#endif
