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

#include <gz/math/Pose3.hh>
#include <gz/math/Vector3.hh>

#include "gz/rendering/PixelFormat.hh"
#include "gz/rendering/o3de/O3deCamera.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
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
      default:
        return false;
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
  // M0 stub: no Atom frame is rendered yet. A later milestone will drive
  // an Atom render-to-texture frame here.
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

  // Gather the camera pose + projection (gz world frame).
  O3deCameraData camData;
  const math::Pose3d camPose = this->camera->WorldPose();
  camData.pos[0] = camPose.Pos().X();
  camData.pos[1] = camPose.Pos().Y();
  camData.pos[2] = camPose.Pos().Z();
  camData.quat[0] = camPose.Rot().W();
  camData.quat[1] = camPose.Rot().X();
  camData.quat[2] = camPose.Rot().Y();
  camData.quat[3] = camPose.Rot().Z();
  camData.hfov = this->camera->HFOV().Radian();
  camData.nearClip = this->camera->NearClipPlane();
  camData.farClip = this->camera->FarClipPlane();

  // Walk the scene's visuals and collect the drawable primitives with their
  // world pose, scale and diffuse color.
  std::vector<O3deShapeData> shapes;
  ScenePtr scene = this->camera->Scene();
  if (scene)
  {
    for (unsigned int i = 0u; i < scene->VisualCount(); ++i)
    {
      VisualPtr visual = scene->VisualByIndex(i);
      O3deVisualPtr o3deVisual =
          std::dynamic_pointer_cast<O3deVisual>(visual);
      if (!o3deVisual)
        continue;

      const math::Pose3d wp = o3deVisual->WorldPose();
      const math::Vector3d ws = o3deVisual->WorldScale();

      for (unsigned int j = 0u; j < o3deVisual->GeometryCount(); ++j)
      {
        O3deGeometryPtr geom = std::dynamic_pointer_cast<O3deGeometry>(
            o3deVisual->GeometryByIndex(j));
        if (!geom)
          continue;

        O3deShapeData shape;
        if (!ToBackendType(geom->Type(), shape.type))
          continue;

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
        shapes.push_back(shape);
      }
    }
  }

  const unsigned int channels = (_image.Format() == PF_R8G8B8) ? 3u : 4u;

  // The backend produces tightly packed RGBA8888.
  if (channels == 4u)
  {
    if (!O3deBackend::Instance().RenderFrame(camData, shapes, w, h, data))
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
    if (!O3deBackend::Instance().RenderFrame(camData, shapes, w, h,
        rgba.data()))
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
