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
#ifndef GZ_RENDERING_O3DE_O3DEMATERIAL_HH_
#define GZ_RENDERING_O3DE_O3DEMATERIAL_HH_

#include "gz/rendering/base/BaseMaterial.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Material class.
    ///
    /// For the M0 stub the color/PBR state is held by BaseMaterial; no
    /// Atom material asset is created yet. A later milestone will map the
    /// diffuse/ambient color onto AuxGeom draws.
    class GZ_RENDERING_O3DE_VISIBLE O3deMaterial :
      public BaseMaterial<O3deObject>
    {
      /// \brief Constructor
      protected: O3deMaterial();

      /// \brief Destructor
      public: virtual ~O3deMaterial();

      /// \brief Make the scene our friend so it can create materials
      private: friend class O3deScene;
    };
    }
  }
}
#endif
