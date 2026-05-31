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
#include <gz/common/Console.hh>

#include "gz/rendering/o3de/O3deArrowVisual.hh"
#include "gz/rendering/o3de/O3deAxisVisual.hh"
#include "gz/rendering/o3de/O3deCamera.hh"
#include "gz/rendering/o3de/O3deCapsule.hh"
#include "gz/rendering/o3de/O3deFrustumVisual.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deGrid.hh"
#include "gz/rendering/o3de/O3deLight.hh"
#include "gz/rendering/o3de/O3deMaterial.hh"
#include "gz/rendering/o3de/O3deMesh.hh"
#include "gz/rendering/o3de/O3deRayQuery.hh"
#include "gz/rendering/o3de/O3deRenderEngine.hh"
#include "gz/rendering/o3de/O3deScene.hh"
#include "gz/rendering/o3de/O3deStorage.hh"
#include "gz/rendering/o3de/O3deVisual.hh"
#include "gz/rendering/o3de/O3deWireBox.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deScene::O3deScene(unsigned int _id, const std::string &_name) :
  BaseScene(_id, _name)
{
}

//////////////////////////////////////////////////
O3deScene::~O3deScene()
{
}

//////////////////////////////////////////////////
void O3deScene::Fini()
{
}

//////////////////////////////////////////////////
RenderEngine *O3deScene::Engine() const
{
  return O3deRenderEngine::Instance();
}

//////////////////////////////////////////////////
VisualPtr O3deScene::RootVisual() const
{
  return this->rootVisual;
}

//////////////////////////////////////////////////
math::Color O3deScene::AmbientLight() const
{
  return this->ambientLight;
}

//////////////////////////////////////////////////
void O3deScene::SetAmbientLight(const math::Color &_color)
{
  this->ambientLight = _color;
}

//////////////////////////////////////////////////
bool O3deScene::LoadImpl()
{
  return true;
}

//////////////////////////////////////////////////
bool O3deScene::InitImpl()
{
  this->CreateStores();
  this->CreateRootVisual();

  // Pre-register the "Default/TransRed", "Default/TransGreen" and
  // "Default/TransBlue" materials BaseAxisVisual::Init looks up by name to
  // colour the X/Y/Z arrows. Without these the SetMaterial(name) call
  // silently no-ops and the axes render at the default geometry colour.
  // "Trans" is the gz-rendering convention -- the alpha is 0.5 to match how
  // other backends draw them.
  for (const auto &kv : std::initializer_list<std::pair<const char *, math::Color>>{
       {"Default/TransRed",   math::Color(1.0f, 0.0f, 0.0f, 0.5f)},
       {"Default/TransGreen", math::Color(0.0f, 1.0f, 0.0f, 0.5f)},
       {"Default/TransBlue",  math::Color(0.0f, 0.0f, 1.0f, 0.5f)}})
  {
    if (!this->MaterialRegistered(kv.first))
    {
      MaterialPtr m = this->CreateMaterial(kv.first);
      if (m)
      {
        m->SetAmbient(kv.second);
        m->SetDiffuse(kv.second);
        m->SetEmissive(kv.second);
        m->SetSpecular(kv.second);
      }
    }
  }
  return true;
}

//////////////////////////////////////////////////
LightStorePtr O3deScene::Lights() const
{
  return this->lights;
}

//////////////////////////////////////////////////
SensorStorePtr O3deScene::Sensors() const
{
  return this->sensors;
}

//////////////////////////////////////////////////
VisualStorePtr O3deScene::Visuals() const
{
  return this->visuals;
}

//////////////////////////////////////////////////
MaterialMapPtr O3deScene::Materials() const
{
  return this->materials;
}

//////////////////////////////////////////////////
DirectionalLightPtr O3deScene::CreateDirectionalLightImpl(unsigned int _id,
    const std::string &_name)
{
  O3deDirectionalLightPtr light(new O3deDirectionalLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
PointLightPtr O3deScene::CreatePointLightImpl(unsigned int _id,
    const std::string &_name)
{
  O3dePointLightPtr light(new O3dePointLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
SpotLightPtr O3deScene::CreateSpotLightImpl(unsigned int _id,
    const std::string &_name)
{
  O3deSpotLightPtr light(new O3deSpotLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
CameraPtr O3deScene::CreateCameraImpl(unsigned int _id,
    const std::string &_name)
{
  O3deCameraPtr camera(new O3deCamera);
  bool result = this->InitObject(camera, _id, _name);
  return (result) ? camera : nullptr;
}

//////////////////////////////////////////////////
DepthCameraPtr O3deScene::CreateDepthCameraImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Depth camera not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
VisualPtr O3deScene::CreateVisualImpl(unsigned int _id,
    const std::string &_name)
{
  O3deVisualPtr visual(new O3deVisual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
ArrowVisualPtr O3deScene::CreateArrowVisualImpl(unsigned int _id,
    const std::string &_name)
{
  O3deArrowVisualPtr arrow(new O3deArrowVisual);
  bool result = this->InitObject(arrow, _id, _name);
  return (result) ? arrow : nullptr;
}

//////////////////////////////////////////////////
AxisVisualPtr O3deScene::CreateAxisVisualImpl(unsigned int _id,
    const std::string &_name)
{
  O3deAxisVisualPtr axis(new O3deAxisVisual);
  bool result = this->InitObject(axis, _id, _name);
  return (result) ? axis : nullptr;
}

//////////////////////////////////////////////////
COMVisualPtr O3deScene::CreateCOMVisualImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "COM visual not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
InertiaVisualPtr O3deScene::CreateInertiaVisualImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Inertia visual not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
JointVisualPtr O3deScene::CreateJointVisualImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Joint visual not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
LightVisualPtr O3deScene::CreateLightVisualImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Light visual not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreateBoxImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateGeometryImpl(_id, _name, O3deGeometry::GeometryType::BOX);
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreateConeImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateGeometryImpl(_id, _name, O3deGeometry::GeometryType::CONE);
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreateCylinderImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateGeometryImpl(_id, _name,
      O3deGeometry::GeometryType::CYLINDER);
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreatePlaneImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateGeometryImpl(_id, _name, O3deGeometry::GeometryType::PLANE);
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreateSphereImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateGeometryImpl(_id, _name,
      O3deGeometry::GeometryType::SPHERE);
}

//////////////////////////////////////////////////
MeshPtr O3deScene::CreateMeshImpl(unsigned int _id,
    const std::string &_name, const MeshDescriptor &/*_desc*/)
{
  // M5 Phase B: return a valid no-op Mesh so callers funnelling through
  // Scene::CreateMesh -- in particular BaseArrowVisual::Init's rotation
  // ring -- can attach a mesh geometry without crashing. The mesh contributes
  // no AuxGeom draws (geometry type stays OTHER, so the per-frame gather
  // skips it). Real mesh import is M9 in the post-beta1 roadmap.
  O3deMeshPtr mesh(new O3deMesh);
  bool result = this->InitObject(mesh, _id, _name);
  return (result) ? mesh : nullptr;
}

//////////////////////////////////////////////////
CapsulePtr O3deScene::CreateCapsuleImpl(unsigned int _id,
    const std::string &_name)
{
  O3deCapsulePtr capsule(new O3deCapsule);
  bool result = this->InitObject(capsule, _id, _name);
  return (result) ? capsule : nullptr;
}

//////////////////////////////////////////////////
GridPtr O3deScene::CreateGridImpl(unsigned int _id,
    const std::string &_name)
{
  O3deGridPtr grid(new O3deGrid);
  bool result = this->InitObject(grid, _id, _name);
  return (result) ? grid : nullptr;
}

//////////////////////////////////////////////////
MarkerPtr O3deScene::CreateMarkerImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Marker not supported by: " << this->Engine()->Name() << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
LidarVisualPtr O3deScene::CreateLidarVisualImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Lidar visual not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
FrustumVisualPtr O3deScene::CreateFrustumVisualImpl(unsigned int _id,
    const std::string &_name)
{
  O3deFrustumVisualPtr frustum(new O3deFrustumVisual);
  bool result = this->InitObject(frustum, _id, _name);
  return (result) ? frustum : nullptr;
}

//////////////////////////////////////////////////
HeightmapPtr O3deScene::CreateHeightmapImpl(unsigned int /*_id*/,
    const std::string &/*_name*/, const HeightmapDescriptor &/*_desc*/)
{
  gzerr << "Heightmap not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
WireBoxPtr O3deScene::CreateWireBoxImpl(unsigned int _id,
    const std::string &_name)
{
  O3deWireBoxPtr wireBox(new O3deWireBox);
  bool result = this->InitObject(wireBox, _id, _name);
  return (result) ? wireBox : nullptr;
}

//////////////////////////////////////////////////
MaterialPtr O3deScene::CreateMaterialImpl(unsigned int _id,
    const std::string &_name)
{
  O3deMaterialPtr material(new O3deMaterial);
  bool result = this->InitObject(material, _id, _name);
  return (result) ? material : nullptr;
}

//////////////////////////////////////////////////
RenderTexturePtr O3deScene::CreateRenderTextureImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Render texture not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
RenderWindowPtr O3deScene::CreateRenderWindowImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  gzerr << "Render window not supported by: " << this->Engine()->Name()
        << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
RayQueryPtr O3deScene::CreateRayQueryImpl(unsigned int _id,
    const std::string &_name)
{
  O3deRayQueryPtr rayQuery(new O3deRayQuery);
  bool result = this->InitObject(rayQuery, _id, _name);
  return (result) ? rayQuery : nullptr;
}

//////////////////////////////////////////////////
GeometryPtr O3deScene::CreateGeometryImpl(unsigned int _id,
    const std::string &_name, O3deGeometry::GeometryType _type)
{
  O3deGeometryPtr geometry(new O3deGeometry);
  geometry->SetType(_type);
  bool result = this->InitObject(geometry, _id, _name);
  return (result) ? geometry : nullptr;
}

//////////////////////////////////////////////////
bool O3deScene::InitObject(O3deObjectPtr _object, unsigned int _id,
    const std::string &_name)
{
  // assign needed variables
  _object->id = _id;
  _object->name = _name;
  _object->scene = this->SharedThis();

  // initialize object
  _object->Load();
  _object->Init();

  return true;
}

//////////////////////////////////////////////////
void O3deScene::CreateRootVisual()
{
  if (this->rootVisual)
    return;

  // create unregistered visual
  this->rootVisual = O3deVisualPtr(new O3deVisual);
  unsigned int rootId = this->CreateObjectId();
  std::string rootName = this->CreateObjectName(rootId, "_ROOT_");

  // check if root visual created successfully
  if (!this->InitObject(this->rootVisual, rootId, rootName))
  {
    gzerr << "Unable to create root visual" << std::endl;
    this->rootVisual = nullptr;
  }
}

//////////////////////////////////////////////////
void O3deScene::CreateStores()
{
  this->lights = O3deLightStorePtr(new O3deLightStore);
  this->sensors = O3deSensorStorePtr(new O3deSensorStore);
  this->visuals = O3deVisualStorePtr(new O3deVisualStore);
  this->materials = O3deMaterialMapPtr(new O3deMaterialMap);
}

//////////////////////////////////////////////////
O3deScenePtr O3deScene::SharedThis()
{
  ScenePtr sharedBase = this->shared_from_this();
  return std::dynamic_pointer_cast<O3deScene>(sharedBase);
}
