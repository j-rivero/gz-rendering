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
#include "gz/rendering/o3de/O3deNode.hh"
#include "gz/rendering/o3de/O3deScene.hh"
#include "gz/rendering/o3de/O3deSensor.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {

    typedef BaseGeometryStore<O3deGeometry>   O3deGeometryStore;
    typedef BaseLightStore<O3deLight>         O3deLightStore;
    typedef BaseNodeStore<O3deNode>           O3deNodeStore;
    typedef BaseSceneStore<O3deScene>         O3deSceneStore;
    typedef BaseSensorStore<O3deSensor>       O3deSensorStore;
    typedef BaseVisualStore<O3deVisual>       O3deVisualStore;

    typedef std::shared_ptr<O3deGeometryStore> O3deGeometryStorePtr;
    typedef std::shared_ptr<O3deLightStore>    O3deLightStorePtr;
    typedef std::shared_ptr<O3deNodeStore>     O3deNodeStorePtr;
    typedef std::shared_ptr<O3deSceneStore>    O3deSceneStorePtr;
    typedef std::shared_ptr<O3deSensorStore>   O3deSensorStorePtr;
    typedef std::shared_ptr<O3deVisualStore>   O3deVisualStorePtr;
    }
  }
}
#endif
