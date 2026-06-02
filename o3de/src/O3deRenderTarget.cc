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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <gz/math/AxisAlignedBox.hh>
#include <gz/math/Pose3.hh>
#include <gz/math/Vector3.hh>

#include "gz/rendering/Capsule.hh"
#include "gz/rendering/FrustumVisual.hh"
#include "gz/rendering/Grid.hh"
#include "gz/rendering/Light.hh"
#include "gz/rendering/PixelFormat.hh"
#include "gz/rendering/WireBox.hh"
#include "gz/rendering/o3de/O3deCamera.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deMesh.hh"  // M9-B: detect real mesh geometries
#include "gz/rendering/o3de/O3deLight.hh"
#include "gz/rendering/o3de/O3deMaterial.hh"
#include "gz/rendering/o3de/O3deRenderTarget.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

#include "O3deBackend.hh"

using namespace gz;
using namespace rendering;

namespace
{
  /// \brief Map the gz primitive type to the backend's shape type.
  /// \return False if this geometry is not a drawable primitive.
  bool ToBackendType(O3deGeometry::GeometryType _in,
      O3deShapeData::Type &_out)
  {
    switch (_in)
    {
      case O3deGeometry::GeometryType::BOX:
        _out = O3deShapeData::Type::BOX; return true;
      case O3deGeometry::GeometryType::SPHERE:
        _out = O3deShapeData::Type::SPHERE; return true;
      case O3deGeometry::GeometryType::CYLINDER:
        _out = O3deShapeData::Type::CYLINDER; return true;
      case O3deGeometry::GeometryType::CONE:
        _out = O3deShapeData::Type::CONE; return true;
      case O3deGeometry::GeometryType::PLANE:
        _out = O3deShapeData::Type::PLANE; return true;
      case O3deGeometry::GeometryType::GRID:
        _out = O3deShapeData::Type::GRID; return true;
      case O3deGeometry::GeometryType::WIREBOX:
        _out = O3deShapeData::Type::WIREBOX; return true;
      case O3deGeometry::GeometryType::CAPSULE:
        _out = O3deShapeData::Type::CAPSULE; return true;
      default:
        return false;
    }
  }

  /// \brief Collect the camera pose/projection, drawable primitives + lights
  /// (world pose, scale, diffuse colour, intensity, attenuation) from the
  /// camera's scene, in gz world frame.
  /// Shared by Copy() (CPU readback) and Render() (native interop).
  void GatherFrame(O3deCamera *_camera, O3deCameraData &_camData,
      std::vector<O3deShapeData> &_shapes,
      std::vector<O3deLightData> &_lights,
      std::vector<O3deMeshData> &_meshes)
  {
    const math::Pose3d camPose = _camera->WorldPose();
    _camData.pos[0] = camPose.Pos().X();
    _camData.pos[1] = camPose.Pos().Y();
    _camData.pos[2] = camPose.Pos().Z();
    _camData.quat[0] = camPose.Rot().W();
    _camData.quat[1] = camPose.Rot().X();
    _camData.quat[2] = camPose.Rot().Y();
    _camData.quat[3] = camPose.Rot().Z();
    _camData.hfov = _camera->HFOV().Radian();
    _camData.nearClip = _camera->NearClipPlane();
    _camData.farClip = _camera->FarClipPlane();

    ScenePtr scene = _camera->Scene();
    if (!scene)
      return;
    for (unsigned int i = 0u; i < scene->VisualCount(); ++i)
    {
      O3deVisualPtr o3deVisual =
          std::dynamic_pointer_cast<O3deVisual>(scene->VisualByIndex(i));
      if (!o3deVisual)
        continue;

      // Honour SetVisible(false): gz-gui plugins (e.g. InteractiveViewControl)
      // hide their reference visuals when not interacting.
      if (!o3deVisual->Visible())
        continue;

      const math::Pose3d wp = o3deVisual->WorldPose();
      const math::Vector3d ws = o3deVisual->WorldScale();

      // FrustumVisual is a Visual subclass that carries the frustum params
      // directly (no Geometry attached). Emit one FRUSTUM shape per matched
      // visual so the backend can draw the wireframe.
      if (auto frustum = std::dynamic_pointer_cast<FrustumVisual>(
              scene->VisualByIndex(i)))
      {
        O3deShapeData shape;
        shape.type = O3deShapeData::Type::FRUSTUM;
        shape.pos[0] = wp.Pos().X();
        shape.pos[1] = wp.Pos().Y();
        shape.pos[2] = wp.Pos().Z();
        shape.quat[0] = wp.Rot().W();
        shape.quat[1] = wp.Rot().X();
        shape.quat[2] = wp.Rot().Y();
        shape.quat[3] = wp.Rot().Z();
        shape.frustumNear = frustum->NearClipPlane();
        shape.frustumFar = frustum->FarClipPlane();
        shape.frustumHFov = frustum->HFOV().Radian();
        shape.frustumAspectRatio = frustum->AspectRatio();
        // Default to a transparent-blue ray colour, matching the
        // "Frustum/BlueRay" material the BaseFrustumVisual::Init registers.
        if (MaterialPtr mat = frustum->Material())
        {
          const math::Color c = mat->Diffuse();
          shape.color[0] = c.R();
          shape.color[1] = c.G();
          shape.color[2] = c.B();
          shape.color[3] = c.A();
        }
        else
        {
          shape.color[0] = 0.2f; shape.color[1] = 0.6f;
          shape.color[2] = 1.0f; shape.color[3] = 1.0f;
        }
        _shapes.push_back(shape);
      }

      for (unsigned int j = 0u; j < o3deVisual->GeometryCount(); ++j)
      {
        O3deGeometryPtr geom = std::dynamic_pointer_cast<O3deGeometry>(
            o3deVisual->GeometryByIndex(j));
        if (!geom)
          continue;

        // M9-B: a real mesh geometry renders via the MeshFeatureProcessor, not
        // AuxGeom. Its geometry was registered with the backend at create time
        // (O3deScene::CreateMeshImpl -> RegisterMesh) keyed by the mesh's id;
        // here we only emit the per-frame world transform + tint. Meshes have
        // GeometryType OTHER, so they fall through ToBackendType below -- detect
        // the concrete type instead.
        if (auto o3deMesh = std::dynamic_pointer_cast<O3deMesh>(geom))
        {
          O3deMeshData md;
          md.id = o3deMesh->Id();
          md.pos[0] = wp.Pos().X();
          md.pos[1] = wp.Pos().Y();
          md.pos[2] = wp.Pos().Z();
          md.quat[0] = wp.Rot().W();
          md.quat[1] = wp.Rot().X();
          md.quat[2] = wp.Rot().Y();
          md.quat[3] = wp.Rot().Z();
          md.scale[0] = ws.X();
          md.scale[1] = ws.Y();
          md.scale[2] = ws.Z();
          if (MaterialPtr mat = o3deMesh->Material())
          {
            const math::Color c = mat->Diffuse();
            md.color[0] = c.R();
            md.color[1] = c.G();
            md.color[2] = c.B();
            md.color[3] = c.A();
          }
          _meshes.push_back(md);
          continue;
        }

        O3deShapeData shape;
        if (!ToBackendType(geom->Type(), shape.type))
          continue;

        // Grid / wire-box carry extra parameters beyond the world pose/scale.
        if (shape.type == O3deShapeData::Type::GRID)
        {
          if (auto grid = std::dynamic_pointer_cast<Grid>(
                  o3deVisual->GeometryByIndex(j)))
          {
            shape.cellCount = static_cast<int>(grid->CellCount());
            shape.cellLength = grid->CellLength();
            shape.verticalCellCount =
                static_cast<int>(grid->VerticalCellCount());
          }
        }
        else if (shape.type == O3deShapeData::Type::WIREBOX)
        {
          if (auto wireBox = std::dynamic_pointer_cast<WireBox>(
                  o3deVisual->GeometryByIndex(j)))
          {
            const math::AxisAlignedBox b = wireBox->Box();
            shape.boxMin[0] = b.Min().X();
            shape.boxMin[1] = b.Min().Y();
            shape.boxMin[2] = b.Min().Z();
            shape.boxMax[0] = b.Max().X();
            shape.boxMax[1] = b.Max().Y();
            shape.boxMax[2] = b.Max().Z();
          }
        }
        else if (shape.type == O3deShapeData::Type::CAPSULE)
        {
          if (auto capsule = std::dynamic_pointer_cast<Capsule>(
                  o3deVisual->GeometryByIndex(j)))
          {
            shape.capsuleRadius = capsule->Radius();
            shape.capsuleLength = capsule->Length();
          }
        }

        shape.pos[0] = wp.Pos().X();
        shape.pos[1] = wp.Pos().Y();
        shape.pos[2] = wp.Pos().Z();
        shape.quat[0] = wp.Rot().W();
        shape.quat[1] = wp.Rot().X();
        shape.quat[2] = wp.Rot().Y();
        shape.quat[3] = wp.Rot().Z();
        shape.scale[0] = ws.X();
        shape.scale[1] = ws.Y();
        shape.scale[2] = ws.Z();

        if (MaterialPtr mat = geom->Material())
        {
          const math::Color c = mat->Diffuse();
          shape.color[0] = c.R();
          shape.color[1] = c.G();
          shape.color[2] = c.B();
          shape.color[3] = c.A();
        }
        _shapes.push_back(shape);
      }
    }

    // M6 lights: walk the scene's light store and produce one O3deLightData
    // per gz light. The backend's SubmitLights() acquires/releases Atom FP
    // handles keyed off each gz id (which is stable across frames). Types are
    // disambiguated via dynamic_pointer_cast in the order DirectionalLight ->
    // SpotLight -> PointLight; only the type-specific properties (direction,
    // cone angles, attenuation range) are read from the matching subclass.
    for (unsigned int i = 0u; i < scene->LightCount(); ++i)
    {
      LightPtr light = scene->LightByIndex(i);
      O3deLightPtr o3deLight = std::dynamic_pointer_cast<O3deLight>(light);
      if (!o3deLight)
        continue;

      const math::Pose3d lp = o3deLight->WorldPose();
      const math::Color c = o3deLight->DiffuseColor();

      O3deLightData data;
      data.id = o3deLight->Id();
      data.pos[0] = lp.Pos().X();
      data.pos[1] = lp.Pos().Y();
      data.pos[2] = lp.Pos().Z();
      data.quat[0] = lp.Rot().W();
      data.quat[1] = lp.Rot().X();
      data.quat[2] = lp.Rot().Y();
      data.quat[3] = lp.Rot().Z();
      data.diffuseColor[0] = c.R();
      data.diffuseColor[1] = c.G();
      data.diffuseColor[2] = c.B();
      data.intensity = o3deLight->Intensity();
      data.attenRange = o3deLight->AttenuationRange();

      if (auto dir = std::dynamic_pointer_cast<DirectionalLight>(light))
      {
        data.type = O3deLightData::Type::DIRECTIONAL;
        const math::Vector3d d = dir->Direction();
        data.dir[0] = d.X(); data.dir[1] = d.Y(); data.dir[2] = d.Z();
      }
      else if (auto spot = std::dynamic_pointer_cast<SpotLight>(light))
      {
        data.type = O3deLightData::Type::SPOT;
        const math::Vector3d d = spot->Direction();
        data.dir[0] = d.X(); data.dir[1] = d.Y(); data.dir[2] = d.Z();
        data.innerAngle = spot->InnerAngle().Radian();
        data.outerAngle = spot->OuterAngle().Radian();
      }
      else if (std::dynamic_pointer_cast<PointLight>(light))
      {
        data.type = O3deLightData::Type::POINT;
      }
      else
      {
        // Unknown light subclass -- skip rather than register it as a default
        // directional light at the origin.
        continue;
      }
      _lights.push_back(data);
    }
  }
}

//////////////////////////////////////////////////
O3deRenderTarget::O3deRenderTarget()
{
}

//////////////////////////////////////////////////
O3deRenderTarget::~O3deRenderTarget()
{
}

//////////////////////////////////////////////////
void O3deRenderTarget::Render()
{
  // Native Vulkan->Vulkan path: gz-gui drives Camera::Update() -> Render() each
  // frame (Copy() is the separate OpenGL CPU-readback path it does NOT call
  // here). Render an offscreen Atom frame into the exportable shared image so the
  // consumer samples the live scene zero-copy. No-op unless the backend was
  // built with -DGZ_O3DE_INTEROP=ON and started with GZ_O3DE_INTEROP set.
  if (nullptr == this->camera)
    return;
  const uint32_t w = this->camera->ImageWidth();
  const uint32_t h = this->camera->ImageHeight();
  if (w == 0u || h == 0u)
    return;

  O3deCameraData camData;
  std::vector<O3deShapeData> shapes;
  std::vector<O3deLightData> lights;
  std::vector<O3deMeshData> meshes;
  GatherFrame(this->camera, camData, shapes, lights, meshes);
  O3deBackend::Instance().RenderFrameForInterop(
      camData, shapes, lights, meshes, w, h);
}

//////////////////////////////////////////////////
void O3deRenderTarget::Copy(Image &_image) const
{
  // Drive a real O3DE/Atom offscreen frame and read it back into the gz Image
  // (already sized by the caller to the camera resolution). This is the
  // CPU-readback bridge gz-gui's MinimalScene fallback relies on.
  const unsigned int w = _image.Width();
  const unsigned int h = _image.Height();
  unsigned char *data = static_cast<unsigned char *>(_image.Data());

  {
    static int n = 0;
    if (n++ < 5)
      std::fprintf(stderr,
          "[gz-o3de] Copy() call %d: w=%u h=%u data=%p camera=%p fmt=%d\n",
          n, w, h, static_cast<void *>(data),
          static_cast<void *>(this->camera),
          static_cast<int>(_image.Format()));
  }

  if (nullptr == data || w == 0u || h == 0u || nullptr == this->camera)
    return;

  // Gather the camera pose + projection, drawable primitives and lights (gz
  // world frame).
  O3deCameraData camData;
  std::vector<O3deShapeData> shapes;
  std::vector<O3deLightData> lights;
  std::vector<O3deMeshData> meshes;
  GatherFrame(this->camera, camData, shapes, lights, meshes);

  const unsigned int channels = (_image.Format() == PF_R8G8B8) ? 3u : 4u;

  // The backend produces tightly packed RGBA8888.
  if (channels == 4u)
  {
    if (!O3deBackend::Instance().RenderFrame(camData, shapes, lights, meshes,
        w, h, data))
    {
      // Leave the caller's buffer untouched on failure.
      return;
    }

    // Debug aid: dump the first rendered frame so we can inspect what the
    // engine actually produced (a file, not a screen grab).
    static bool dumped = false;
    if (!dumped && std::getenv("GZ_O3DE_DUMP_FRAME"))
    {
      dumped = true;
      std::ofstream f("/tmp/gz_gui_frame.ppm", std::ios::binary);
      f << "P6\n" << w << " " << h << "\n255\n";
      for (unsigned int i = 0u; i < w * h; ++i)
      {
        f.put(static_cast<char>(data[i * 4u + 0u]));
        f.put(static_cast<char>(data[i * 4u + 1u]));
        f.put(static_cast<char>(data[i * 4u + 2u]));
      }
      std::fprintf(stderr, "[gz-o3de] dumped first frame %ux%u -> "
          "/tmp/gz_gui_frame.ppm (shapes=%zu)\n", w, h, shapes.size());
    }
  }
  else
  {
    // PF_R8G8B8: render into a temporary RGBA buffer, then drop alpha.
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(w) * h * 4u, 0u);
    if (!O3deBackend::Instance().RenderFrame(camData, shapes, lights, meshes,
        w, h, rgba.data()))
      return;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
    {
      data[i * 3u + 0u] = rgba[i * 4u + 0u];
      data[i * 3u + 1u] = rgba[i * 4u + 1u];
      data[i * 3u + 2u] = rgba[i * 4u + 2u];
    }
  }
}

//////////////////////////////////////////////////
void O3deRenderTarget::Destroy()
{
}

//////////////////////////////////////////////////
void O3deRenderTarget::RebuildImpl()
{
  // M0 stub: nothing to rebuild yet.
}
