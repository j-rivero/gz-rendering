/*
 * Copyright (C) 2026 Open Source Robotics Foundation
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

// M12: real gz materials -> Atom PBR, 100% through the public gz-rendering
// API. No demo injection (GZ_O3DE_DEMO_SHAPES stays unset), no Qt, no
// interop: the scene below is built with CreateMesh/CreateMaterial/
// CreatePointLight and rendered through the CPU-readback path
// (Camera::Capture -> O3deRenderTarget::Copy), then saved as a PNG.
//
// Usage: pbr_materials [albedo.png] [output.png]
// (run via pbr_materials.sh, which sets up the plugin + Atom runtime env)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <gz/common/Image.hh>

#include <gz/rendering/Camera.hh>
#include <gz/rendering/Image.hh>
#include <gz/rendering/Light.hh>
#include <gz/rendering/Material.hh>
#include <gz/rendering/Mesh.hh>
#include <gz/rendering/MeshDescriptor.hh>
#include <gz/rendering/RenderEngine.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/Scene.hh>
#include <gz/rendering/Visual.hh>

using namespace gz;
using namespace gz::rendering;

namespace
{

/// \brief One unit_sphere visual with the given material at _pos.
VisualPtr AddSphere(ScenePtr _scene, VisualPtr _root,
    const std::string &_name, const math::Vector3d &_pos, MaterialPtr _mat)
{
  MeshDescriptor descriptor("unit_sphere");
  MeshPtr mesh = _scene->CreateMesh(descriptor);
  if (!mesh)
  {
    std::fprintf(stderr, "[pbr_materials] CreateMesh(unit_sphere) FAILED\n");
    return nullptr;
  }
  mesh->SetMaterial(_mat);
  VisualPtr v = _scene->CreateVisual(_name);
  v->AddGeometry(mesh);
  v->SetLocalPosition(_pos);
  _root->AddChild(v);
  return v;
}

}  // namespace

int main(int _argc, char **_argv)
{
  const std::string texPath = _argc > 1 ? _argv[1] : "";
  const std::string outPath =
      _argc > 2 ? _argv[2] : "/tmp/m12_pbr_materials.png";

  RenderEngine *engine = rendering::engine("o3de");
  if (!engine)
  {
    std::fprintf(stderr, "[pbr_materials] engine 'o3de' not found -- check "
        "GZ_RENDERING_PLUGIN_PATH (run via pbr_materials.sh)\n");
    return 1;
  }

  ScenePtr scene = engine->CreateScene("scene");
  if (!scene)
  {
    std::fprintf(stderr, "[pbr_materials] CreateScene failed\n");
    return 1;
  }
  scene->SetAmbientLight(0.3, 0.3, 0.3);
  VisualPtr root = scene->RootVisual();

  // Ground: a flattened unit_box with a rough dielectric grey.
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.40, 0.42, 0.45);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.9f);
    MeshDescriptor desc("unit_box");
    MeshPtr mesh = scene->CreateMesh(desc);
    if (!mesh)
    {
      std::fprintf(stderr, "[pbr_materials] CreateMesh(unit_box) FAILED\n");
      return 1;
    }
    mesh->SetMaterial(mat);
    VisualPtr ground = scene->CreateVisual("ground");
    ground->AddGeometry(mesh);
    ground->SetLocalScale(12.0, 12.0, 0.5);
    ground->SetLocalPosition(0.0, 0.0, -0.25);  // top face at z=0
    root->AddChild(ground);
  }

  // Bottom row: 5-sphere metallic roughness sweep 0.05 -> 0.95 (mirrors the
  // proven M11-A demo scene for direct visual comparison).
  for (int i = 0; i < 5; ++i)
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.95, 0.95, 0.95);
    mat->SetMetalness(1.0f);
    mat->SetRoughness(0.05f + 0.225f * static_cast<float>(i));
    AddSphere(scene, root, "sweep_" + std::to_string(i),
        math::Vector3d(0.0, -2.0 + i, 0.5), mat);
  }

  // Top row, left: gold (warm metal -- needs IBL to read as metal).
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(1.0, 0.77, 0.34);
    mat->SetMetalness(1.0f);
    mat->SetRoughness(0.25f);
    AddSphere(scene, root, "gold", math::Vector3d(0.0, -0.75, 1.6), mat);
  }

  // Top row, right: real albedo texture FILE through Material::SetTexture --
  // the exact path a gz-sim material's base-color map takes (M11-D backend).
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(1.0, 1.0, 1.0);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.6f);
    if (!texPath.empty())
      mat->SetTexture(texPath);
    else
      std::fprintf(stderr, "[pbr_materials] no albedo path given -- "
          "textured sphere will be plain white\n");
    AddSphere(scene, root, "textured", math::Vector3d(0.0, 0.75, 1.6), mat);
  }

  // Lights: key + fill point pair (same values as the backend's PBR_ONLY
  // demo lighting, so captures compare 1:1 against the M11 references).
  {
    PointLightPtr key = scene->CreatePointLight();
    key->SetLocalPosition(-1.5, 1.5, 3.0);
    key->SetDiffuseColor(1.0, 0.97, 0.92);
    key->SetIntensity(220.0);
    key->SetAttenuationRange(18.0);
    root->AddChild(key);

    PointLightPtr fill = scene->CreatePointLight();
    fill->SetLocalPosition(-2.0, -1.5, 1.2);
    fill->SetDiffuseColor(0.6, 0.7, 0.95);
    fill->SetIntensity(70.0);
    fill->SetAttenuationRange(16.0);
    root->AddChild(fill);
  }

  CameraPtr camera = scene->CreateCamera("camera");
  camera->SetLocalPosition(-5.5, 0.0, 1.8);
  camera->SetLocalRotation(0.0, 0.15, 0.0);  // pitch down toward the rows
  camera->SetImageWidth(1280);
  camera->SetImageHeight(720);
  camera->SetAspectRatio(1280.0 / 720.0);
  camera->SetHFOV(1.047);
  root->AddChild(camera);

  // Capture. The first frames ride out Atom's boot/warmup (~3 s); each
  // Capture() drives a full synchronous offscreen frame + CPU readback.
  // Zero the (uninitialized) buffer first: Copy() leaves it untouched on
  // failure, so "still all-zero afterwards" is a sound failed-render probe
  // (the lit scene below never renders fully black).
  Image image = camera->CreateImage();
  const std::size_t bufSize = static_cast<std::size_t>(
      camera->ImageWidth()) * camera->ImageHeight() * 3u;
  std::memset(image.Data<unsigned char>(), 0, bufSize);
  for (int frame = 0; frame < 30; ++frame)
  {
    camera->Capture(image);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  const unsigned char *buf = image.Data<unsigned char>();
  if (std::all_of(buf, buf + bufSize,
      [](unsigned char _b) { return _b == 0u; }))
  {
    std::fprintf(stderr, "[pbr_materials] ERROR: every frame came back "
        "black/untouched -- the backend never rendered (stale plugin .so? "
        "check GZ_RENDERING_PLUGIN_PATH and the engine log above)\n");
    return 1;
  }

  common::Image out;
  out.SetFromData(image.Data<unsigned char>(),
      camera->ImageWidth(), camera->ImageHeight(), common::Image::RGB_INT8);
  out.SavePNG(outPath);
  std::printf("[pbr_materials] wrote %s\n", outPath.c_str());
  // The o3de backend leaves the process via std::quick_exit, which skips
  // stdio flushing -- without this the line above never reaches a log file.
  std::fflush(stdout);

  // No teardown: the o3de backend is a process-wide singleton by design
  // (see O3deBackend docs); process exit is the supported shutdown.
  return 0;
}
