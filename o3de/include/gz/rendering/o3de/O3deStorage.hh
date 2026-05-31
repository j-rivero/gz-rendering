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
#ifndef GZ_RENDERING_O3DE_O3DESTORAGE_HH_
#define GZ_RENDERING_O3DE_O3DESTORAGE_HH_

#include <memory>

#include "gz/rendering/base/BaseStorage.hh"

#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deLight.hh"
#include "gz/rendering/o3de/O3deMesh.hh"
#include "gz/rendering/o3de/O3deNode.hh"
#include "gz/rendering/o3de/O3deScene.hh"
#include "gz/rendering/o3de/O3deSensor.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {

    // The Base*Store typedefs and their Ptr typedefs live in O3deRenderTypes.hh
    // alongside the per-class shared_ptrs, mirroring the Ogre2 pattern -- the
    // SubMeshStorePtr in particular needs to be visible from O3deMesh.hh as a
    // member declaration, which would create a cycle through this header.
    }
  }
}
#endif
