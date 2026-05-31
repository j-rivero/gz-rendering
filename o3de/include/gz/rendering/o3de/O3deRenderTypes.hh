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
#ifndef GZ_RENDERING_O3DE_O3DERENDERTYPES_HH_
#define GZ_RENDERING_O3DE_O3DERENDERTYPES_HH_

#include <memory>

#include "gz/rendering/config.hh"
#include "gz/rendering/base/BaseRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    class O3deArrowVisual;
    class O3deAxisVisual;
    class O3deCamera;
    class O3deCapsule;
    class O3deDirectionalLight;
    class O3deFrustumVisual;
    class O3deGeometry;
    class O3deGrid;
    class O3deLight;
    class O3deMaterial;
    class O3deMesh;
    class O3deNode;
    class O3deObject;
    class O3deSubMesh;
    class O3dePointLight;
    class O3deRayQuery;
    class O3deRenderEngine;
    class O3deRenderTarget;
    class O3deScene;
    class O3deSensor;
    class O3deSpotLight;
    class O3deVisual;
    class O3deWireBox;

    typedef BaseGeometryStore<O3deGeometry>   O3deGeometryStore;
    typedef BaseLightStore<O3deLight>         O3deLightStore;
    typedef BaseNodeStore<O3deNode>           O3deNodeStore;
    typedef BaseSceneStore<O3deScene>         O3deSceneStore;
    typedef BaseSensorStore<O3deSensor>       O3deSensorStore;
    typedef BaseSubMeshStore<O3deSubMesh>     O3deSubMeshStore;
    typedef BaseVisualStore<O3deVisual>       O3deVisualStore;

    typedef BaseMaterialMap<O3deMaterial>     O3deMaterialMap;

    typedef shared_ptr<O3deArrowVisual>       O3deArrowVisualPtr;
    typedef shared_ptr<O3deAxisVisual>        O3deAxisVisualPtr;
    typedef shared_ptr<O3deCamera>            O3deCameraPtr;
    typedef shared_ptr<O3deCapsule>           O3deCapsulePtr;
    typedef shared_ptr<O3deDirectionalLight>  O3deDirectionalLightPtr;
    typedef shared_ptr<O3deFrustumVisual>     O3deFrustumVisualPtr;
    typedef shared_ptr<O3deGeometry>          O3deGeometryPtr;
    typedef shared_ptr<O3deGrid>              O3deGridPtr;
    typedef shared_ptr<O3deLight>             O3deLightPtr;
    typedef shared_ptr<O3deMaterial>          O3deMaterialPtr;
    typedef shared_ptr<O3deMesh>              O3deMeshPtr;
    typedef shared_ptr<O3deNode>              O3deNodePtr;
    typedef shared_ptr<O3deObject>            O3deObjectPtr;
    typedef shared_ptr<O3deSubMesh>           O3deSubMeshPtr;
    typedef shared_ptr<O3dePointLight>        O3dePointLightPtr;
    typedef shared_ptr<O3deRayQuery>          O3deRayQueryPtr;
    typedef shared_ptr<O3deRenderEngine>      O3deRenderEnginePtr;
    typedef shared_ptr<O3deRenderTarget>      O3deRenderTargetPtr;
    typedef shared_ptr<O3deScene>             O3deScenePtr;
    typedef shared_ptr<O3deSensor>            O3deSensorPtr;
    typedef shared_ptr<O3deSpotLight>         O3deSpotLightPtr;
    typedef shared_ptr<O3deVisual>            O3deVisualPtr;
    typedef shared_ptr<O3deWireBox>           O3deWireBoxPtr;

    typedef shared_ptr<O3deGeometryStore>     O3deGeometryStorePtr;
    typedef shared_ptr<O3deLightStore>        O3deLightStorePtr;
    typedef shared_ptr<O3deNodeStore>         O3deNodeStorePtr;
    typedef shared_ptr<O3deSceneStore>        O3deSceneStorePtr;
    typedef shared_ptr<O3deSensorStore>       O3deSensorStorePtr;
    typedef shared_ptr<O3deSubMeshStore>      O3deSubMeshStorePtr;
    typedef shared_ptr<O3deVisualStore>       O3deVisualStorePtr;

    typedef shared_ptr<O3deMaterialMap>       O3deMaterialMapPtr;
    }
  }
}
#endif
