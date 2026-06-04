// M13: view a real mesh asset (GLB/.dae/.obj...) with the material the FILE
// carries, through the public gz-rendering API on the o3de backend -- no
// SetMaterial call, so the backend's mesh-file material auto-apply (M13) is
// what colours it. Renders headless via the CPU-readback path and writes a
// PNG. Promoted from the M13 smoke test that proved the geometry path on the
// jetty Forklift GLB.
//
// Usage: mesh_pbr_viewer <mesh-file> [output.png]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

#include <gz/common/Image.hh>
#include <gz/common/Mesh.hh>
#include <gz/common/MeshManager.hh>

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

int main(int _argc, char **_argv)
{
  if (_argc < 2)
  {
    std::fprintf(stderr, "usage: mesh_pbr_viewer <mesh-file> [out.png]\n");
    return 2;
  }
  const std::string meshPath = _argv[1];
  const std::string outPath =
      _argc > 2 ? _argv[2] : "/tmp/mesh_pbr_viewer.png";

  // Pre-load via gz-common directly so we can print bounds + submesh info
  // before the renderer touches it (and auto-frame the camera from bounds).
  auto *mgr = common::MeshManager::Instance();
  const common::Mesh *cm = mgr->Load(meshPath);
  if (!cm)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] gz-common FAILED to load %s\n",
        meshPath.c_str());
    return 1;
  }
  const math::Vector3d mn = cm->Min(), mx = cm->Max();
  std::fprintf(stderr,
      "[mesh_pbr_viewer] loaded: %u submeshes, %u verts, %u indices, "
      "%u materials\n  bounds min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f)\n",
      cm->SubMeshCount(), cm->VertexCount(), cm->IndexCount(),
      cm->MaterialCount(),
      mn.X(), mn.Y(), mn.Z(), mx.X(), mx.Y(), mx.Z());

  RenderEngine *engine = rendering::engine("o3de");
  if (!engine)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] engine 'o3de' not found\n");
    return 1;
  }
  ScenePtr scene = engine->CreateScene("scene");
  if (!scene)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] CreateScene failed\n");
    return 1;
  }
  scene->SetAmbientLight(0.3, 0.3, 0.3);
  VisualPtr root = scene->RootVisual();

  // The asset, by file path, centred on the origin, resting on the ground.
  // NO SetMaterial: the mesh file's own material must apply (M13).
  MeshDescriptor descriptor(meshPath);
  MeshPtr mesh = scene->CreateMesh(descriptor);
  if (!mesh)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] CreateMesh(%s) FAILED\n",
        meshPath.c_str());
    return 1;
  }
  VisualPtr v = scene->CreateVisual("asset");
  v->AddGeometry(mesh);
  const math::Vector3d centre = (mn + mx) * 0.5;
  v->SetLocalPosition(-centre.X(), -centre.Y(), -mn.Z());
  root->AddChild(v);

  // Ground + the proven key/fill lighting from the pbr_materials example,
  // intensity and distances scaled by the model extent.
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.40, 0.42, 0.45);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.9f);
    MeshDescriptor desc("unit_box");
    MeshPtr g = scene->CreateMesh(desc);
    g->SetMaterial(mat);
    VisualPtr ground = scene->CreateVisual("ground");
    ground->AddGeometry(g);
    const double span = std::max({mx.X() - mn.X(), mx.Y() - mn.Y(), 4.0});
    ground->SetLocalScale(span * 4.0, span * 4.0, 0.5);
    ground->SetLocalPosition(0.0, 0.0, -0.25);
    root->AddChild(ground);
  }
  // Floor the extent so degenerate bounds (point mesh, bad asset) still
  // produce a sensible camera/light setup instead of a black frame.
  const double ext = std::max({mx.X() - mn.X(), mx.Y() - mn.Y(),
      mx.Z() - mn.Z(), 0.1});
  {
    PointLightPtr key = scene->CreatePointLight();
    key->SetLocalPosition(-1.0 * ext, 1.2 * ext, 2.0 * ext);
    key->SetDiffuseColor(1.0, 0.97, 0.92);
    key->SetIntensity(220.0 * ext);
    key->SetAttenuationRange(20.0 * ext);
    root->AddChild(key);
    PointLightPtr fill = scene->CreatePointLight();
    fill->SetLocalPosition(-1.5 * ext, -1.0 * ext, 0.8 * ext);
    fill->SetDiffuseColor(0.6, 0.7, 0.95);
    fill->SetIntensity(70.0 * ext);
    fill->SetAttenuationRange(18.0 * ext);
    root->AddChild(fill);
  }

  // Camera auto-framed from the bounds: back off ~2.2x the largest extent,
  // looking at the model's mid-height.
  CameraPtr camera = scene->CreateCamera("camera");
  const double dist = 2.2 * ext;
  camera->SetLocalPosition(-dist, 0.6 * dist, 0.45 * ext);
  // yaw toward the model (it sits at the origin): atan2(-y, -x)
  const double yaw = std::atan2(-0.6 * dist, dist);
  camera->SetLocalRotation(0.0, 0.05, yaw);
  camera->SetImageWidth(1280);
  camera->SetImageHeight(720);
  camera->SetAspectRatio(1280.0 / 720.0);
  camera->SetHFOV(1.047);
  root->AddChild(camera);

  // 30-frame warmup capture. The buffer starts uninitialized heap memory and
  // Copy() leaves it untouched on failure -- zero it first so the all-zero
  // check below is a reliable "backend never rendered" detector.
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
    std::fprintf(stderr, "[mesh_pbr_viewer] ERROR: backend never rendered\n");
    return 1;
  }
  common::Image out;
  out.SetFromData(image.Data<unsigned char>(),
      camera->ImageWidth(), camera->ImageHeight(), common::Image::RGB_INT8);
  out.SavePNG(outPath);
  std::printf("[mesh_pbr_viewer] wrote %s\n", outPath.c_str());
  // The backend exits via std::quick_exit, which skips stdio flushing.
  std::fflush(stdout);
  return 0;
}
