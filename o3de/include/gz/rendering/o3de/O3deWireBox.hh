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
#ifndef GZ_RENDERING_O3DE_O3DEWIREBOX_HH_
#define GZ_RENDERING_O3DE_O3DEWIREBOX_HH_

#include "gz/rendering/base/BaseWireBox.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of a wireframe box.
    ///
    /// Stores the local axis-aligned box via BaseWireBox; the render backend
    /// reads it when gathering the frame and emits the 12 edges as AuxGeom
    /// lines (see O3deBackend SubmitPrimitives, Type::WIREBOX).
    class GZ_RENDERING_O3DE_VISIBLE O3deWireBox :
      public BaseWireBox<O3deGeometry>
    {
      /// \brief Constructor
      protected: O3deWireBox();

      /// \brief Destructor
      public: virtual ~O3deWireBox();

      /// \brief Make the scene our friend so it can create wire boxes
      private: friend class O3deScene;
    };
    }
  }
}
#endif
