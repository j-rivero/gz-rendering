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

// This is the ONLY translation unit that touches the O3DE/Atom runtime. It is
// compiled with O3DE's compile model (see o3de/src/CMakeLists.txt) and linked
// against the prebuilt O3DE libraries. Everything it exposes to the rest of the
// component goes through the plain-C++ O3deBackend interface.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <AzGameFramework/Application/GameApplication.h>

#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/Math/MatrixUtils.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Matrix3x4.h>
#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/Color.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/std/string/string.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/Pass/Specific/RenderToTexturePass.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomFeatureProcessorInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomDraw.h>
#include <Atom/RPI.Reflect/System/SceneDescriptor.h>
#include <Atom/RPI.Reflect/System/RenderPipelineDescriptor.h>
#include <Atom/Feature/Utils/FrameCaptureBus.h>

#include <AzFramework/Scene/Scene.h>
#include <AzFramework/Scene/SceneSystemInterface.h>

#include "O3deBackend.hh"

namespace gz
{
namespace rendering
{

namespace
{
  // PoC paths. We reuse the vendored O3DE build and the GzAtomPoc cooked-asset
  // project that M1/M2.1 produced. A later milestone will vendor a minimal
  // asset bundle and derive these from the gz install layout. All are
  // overridable via environment variables for portability.
  const char *EnvOr(const char *_envName, const char *_fallback)
  {
    const char *value = std::getenv(_envName);
    return (value && value[0]) ? value : _fallback;
  }
}

/// \brief Private Atom-owning data for O3deBackend.
///
/// All O3DE/Atom calls (bootstrap AND every tick) happen on a single dedicated
/// thread (renderThread). This is required because Atom's AssetManager only
/// self-pumps a synchronous asset load when it runs on the thread that
/// constructed the application (its "main thread"); driving ticks from a
/// different thread (e.g. gz-gui's render thread) deadlocks on a blocking
/// shader load during a pass-tree rebuild. RenderFrame() (called on gz-gui's
/// thread) just hands the camera + shapes across a mutex and waits for the
/// latest readback.
class O3deBackend::Impl
{
  public: AzGameFramework::GameApplication *app = nullptr;
  public: AZ::RPI::ScenePtr scene;
  public: AZStd::shared_ptr<AzFramework::Scene> frameworkScene;
  public: AZ::RPI::RenderPipelinePtr pipeline;
  public: AZ::RPI::ViewPtr view;

  public: const AZStd::string pipelineName = "GzO3deProbePipeline";
  public: uint32_t outputWidth = 0u;
  public: uint32_t outputHeight = 0u;

  // ---- Cross-thread state (guarded by mutex) ------------------------------
  public: std::thread renderThread;
  public: std::mutex mutex;
  public: std::condition_variable inputCv;   // signalled when new input posted
  public: std::condition_variable outputCv;  // signalled when a frame is ready

  // Bootstrap handshake.
  public: bool bootstrapDone = false;
  public: bool bootstrapOk = false;
  public: bool ready = false;       // mirrors bootstrapOk for IsReady()
  public: std::atomic<bool> stop{false};

  // Latest input posted by RenderFrame() (gz-gui thread -> render thread).
  public: O3deCameraData pendingCamera;
  public: std::vector<O3deShapeData> pendingShapes;
  public: uint32_t pendingWidth = 0u;
  public: uint32_t pendingHeight = 0u;
  public: bool haveInput = false;

  // Latest completed frame (render thread -> gz-gui thread).
  public: std::vector<uint8_t> latestFrame;  // tightly packed RGBA8888
  public: uint32_t latestWidth = 0u;
  public: uint32_t latestHeight = 0u;
  public: uint64_t frameSeq = 0u;            // bumped on each completed frame

  // Render-thread-private working copy of the current scene contents.
  public: O3deCameraData camera;
  public: std::vector<O3deShapeData> shapes;
  public: uint32_t reqWidth = 0u;
  public: uint32_t reqHeight = 0u;

  // Capture scratch (render thread only), refilled each RenderOneFrame().
  public: AZStd::vector<uint8_t> captureBuffer;
  public: uint32_t captureWidth = 0u;
  public: uint32_t captureHeight = 0u;
  public: bool captureDone = false;

  // ---- Render-thread-only methods -----------------------------------------
  /// \brief Thread entry point: bootstrap then loop rendering frames.
  public: void RenderThreadMain();
  /// \brief Bring up the GameApplication + Atom runtime (render thread).
  public: bool BootstrapOnThread();
  /// \brief Render one frame from the current camera/shapes into latestFrame.
  public: bool RenderOneFrame();
  /// \brief Build the offscreen scene + render-to-texture pipeline + view.
  public: bool SetupScene(uint32_t _width, uint32_t _height);
  /// \brief (Re)size the offscreen render target if needed.
  public: void EnsureSize(uint32_t _width, uint32_t _height);
  /// \brief Point the RPI view at the current gz camera pose + projection.
  public: void ApplyCamera(uint32_t _width, uint32_t _height);
  /// \brief Submit the current frame's AuxGeom primitives.
  public: void SubmitPrimitives();
};

namespace
{
  // gz cameras look down +X (REP-103: +X fwd, +Y left, +Z up). O3DE views look
  // down +Y (basisY), +Z up. Both are right-handed Z-up, so object world poses
  // pass through unchanged; only the camera needs a local -90 deg yaw to map its
  // forward axis (+X) onto O3DE's (+Y).
  AZ::Quaternion GzQuat(const double (&_q)[4])
  {
    // gz quaternion is (w, x, y, z); AZ::Quaternion ctor is (x, y, z, w).
    return AZ::Quaternion(
        aznumeric_cast<float>(_q[1]), aznumeric_cast<float>(_q[2]),
        aznumeric_cast<float>(_q[3]), aznumeric_cast<float>(_q[0]));
  }

  AZ::Vector3 GzVec(const double (&_v)[3])
  {
    return AZ::Vector3(aznumeric_cast<float>(_v[0]),
        aznumeric_cast<float>(_v[1]), aznumeric_cast<float>(_v[2]));
  }

  // Listens for the "capture fully finished" notification. The readback
  // callback hands us the pixels, but the capture is only recycled back to the
  // FrameCapture idle pool once OnFrameCaptureFinished fires (on a later system
  // tick). Waiting for this before issuing the next capture is what keeps
  // continuous offscreen capture from deadlocking the RHI (see TrackView's
  // AtomOutputFrameCapture, the engine's own continuous-capture user).
  class CaptureFinishedListener
      : public AZ::Render::FrameCaptureNotificationBus::Handler
  {
    public: bool finished = false;
    public: void OnFrameCaptureFinished(
        AZ::Render::FrameCaptureResult /*_result*/,
        const AZStd::string & /*_info*/) override
    {
      this->finished = true;
    }
  };
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::SetupScene(uint32_t _width, uint32_t _height)
{
  auto *rpiSystem = AZ::RPI::RPISystemInterface::Get();
  if (!rpiSystem)
  {
    std::fprintf(stderr, "[gz-o3de] RPISystemInterface unavailable\n");
    return false;
  }

  // The MainPipeline's LightCullingPass dereferences every light feature
  // processor, so all must be present (omitting one segfaults on a null FP).
  // This is the full AtomToolsFramework::PreviewRenderer set (DirectionalLight
  // omitted - known multi-instance flicker bug) plus AuxGeom for our shapes.
  const AZStd::vector<AZStd::string> featureProcessors = {
    "AZ::Render::TransformServiceFeatureProcessor",
    "AZ::Render::MeshFeatureProcessor",
    "AZ::Render::SimplePointLightFeatureProcessor",
    "AZ::Render::SimpleSpotLightFeatureProcessor",
    "AZ::Render::PointLightFeatureProcessor",
    "AZ::Render::DiskLightFeatureProcessor",
    "AZ::Render::CapsuleLightFeatureProcessor",
    "AZ::Render::QuadLightFeatureProcessor",
    "AZ::Render::DecalTextureArrayFeatureProcessor",
    "AZ::Render::ImageBasedLightFeatureProcessor",
    "AZ::Render::PostProcessFeatureProcessor",
    "AZ::Render::SkyBoxFeatureProcessor",
    "AZ::Render::AuxGeomFeatureProcessor",
  };

  AZ::RPI::SceneDescriptor sceneDesc;
  sceneDesc.m_nameId = AZ::Name("GzO3deScene");
  sceneDesc.m_featureProcessorNames.assign(
      featureProcessors.begin(), featureProcessors.end());
  this->scene = AZ::RPI::Scene::CreateScene(sceneDesc);

  auto sceneSystem = AzFramework::SceneSystemInterface::Get();
  if (!sceneSystem)
  {
    std::fprintf(stderr, "[gz-o3de] SceneSystemInterface unavailable\n");
    return false;
  }
  auto createSceneOutcome = sceneSystem->CreateScene("GzO3deFrameworkScene");
  if (!createSceneOutcome)
  {
    std::fprintf(stderr, "[gz-o3de] CreateScene failed: %s\n",
        createSceneOutcome.GetError().c_str());
    return false;
  }
  this->frameworkScene = createSceneOutcome.TakeValue();
  this->frameworkScene->SetSubsystem(this->scene);

  // Offscreen render-to-texture pipeline (no window/swapchain).
  // MainPipelineRenderToTexture is the game pipeline's offscreen variant;
  // ToolsPipelineRenderToTexture pulls in editor-only passes whose templates
  // are not cooked in this project.
  AZ::RPI::RenderPipelineDescriptor pipelineDesc;
  pipelineDesc.m_mainViewTagName = "MainCamera";
  pipelineDesc.m_name = this->pipelineName;
  pipelineDesc.m_rootPassTemplate = "MainPipelineRenderToTexture";
  if (rpiSystem->GetNumScenes() > 0)
  {
    pipelineDesc.m_renderSettings.m_multisampleState =
        rpiSystem->GetApplicationMultisampleState();
  }
  else
  {
    rpiSystem->SetApplicationMultisampleState(
        pipelineDesc.m_renderSettings.m_multisampleState);
  }

  this->pipeline = AZ::RPI::RenderPipeline::CreateRenderPipeline(pipelineDesc);
  this->scene->AddRenderPipeline(this->pipeline);
  this->scene->Activate();
  rpiSystem->RegisterScene(this->scene);

  // Create the view; pose + projection are set per-frame by ApplyCamera().
  this->view = AZ::RPI::View::CreateView(
      AZ::Name("MainCamera"), AZ::RPI::View::UsageCamera);
  this->pipeline->SetDefaultView(this->view);
  this->ApplyCamera(_width, _height);

  // Size the target now and keep the pipeline out of the per-tick render loop
  // until we explicitly request a frame (default RenderEveryTick would render
  // an unsized RTT target and crash the RHI).
  this->EnsureSize(_width, _height);
  this->pipeline->RemoveFromRenderTick();
  return true;
}

//////////////////////////////////////////////////
void O3deBackend::Impl::ApplyCamera(uint32_t _width, uint32_t _height)
{
  if (!this->view)
    return;

  const float aspect = (_height > 0u)
      ? static_cast<float>(_width) / static_cast<float>(_height) : 1.0f;

  // gz reports a horizontal FOV; O3DE wants the vertical FOV.
  const float hFov = static_cast<float>(this->camera.hfov);
  const float vFov = 2.0f * std::atan(std::tan(hFov * 0.5f) / aspect);

  AZ::Matrix4x4 viewToClip;
  AZ::MakePerspectiveFovMatrixRH(viewToClip, vFov, aspect,
      static_cast<float>(this->camera.nearClip),
      static_cast<float>(this->camera.farClip), true);
  this->view->SetViewToClipMatrix(viewToClip);

  // Map the gz camera pose into O3DE: position passes through; orientation gets
  // a local -90 deg yaw so the gz +X forward axis lines up with O3DE's +Y view
  // direction (see GzQuat note above).
  const AZ::Quaternion gzToView =
      AZ::Quaternion::CreateRotationZ(-AZ::Constants::HalfPi);
  const AZ::Quaternion camRot = GzQuat(this->camera.quat) * gzToView;
  const AZ::Transform camTransform =
      AZ::Transform::CreateFromQuaternionAndTranslation(
          camRot, GzVec(this->camera.pos));
  this->view->SetCameraTransform(
      AZ::Matrix3x4::CreateFromTransform(camTransform));
}

//////////////////////////////////////////////////
void O3deBackend::Impl::EnsureSize(uint32_t _width, uint32_t _height)
{
  if (!this->pipeline)
    return;
  if (auto rtt = azrtti_cast<AZ::RPI::RenderToTexturePass *>(
          this->pipeline->GetRootPass().get()))
  {
    rtt->ResizeOutput(_width, _height);
  }
}

//////////////////////////////////////////////////
void O3deBackend::Impl::SubmitPrimitives()
{
  auto auxGeom =
      AZ::RPI::AuxGeomFeatureProcessorInterface::GetDrawQueueForScene(
          this->scene);
  if (!auxGeom)
    return;

  for (const O3deShapeData &shape : this->shapes)
  {
    const AZ::Vector3 pos = GzVec(shape.pos);
    const AZ::Quaternion rot = GzQuat(shape.quat);
    const AZ::Vector3 scale = GzVec(shape.scale);
    const AZ::Color color(shape.color[0], shape.color[1], shape.color[2],
        shape.color[3]);
    // The shape's local +Z axis in world space (for cylinder/cone direction).
    const AZ::Vector3 axisZ = rot.TransformVector(AZ::Vector3::CreateAxisZ());

    switch (shape.type)
    {
      case O3deShapeData::Type::BOX:
      {
        // Unit cube scaled to full extents, then oriented + positioned.
        const AZ::Vector3 half = scale * 0.5f;
        const AZ::Aabb aabb = AZ::Aabb::CreateFromMinMax(-half, half);
        const AZ::Matrix3x4 xform =
            AZ::Matrix3x4::CreateFromQuaternionAndTranslation(rot, pos);
        auxGeom->DrawAabb(aabb, xform, color,
            AZ::RPI::AuxGeomDraw::DrawStyle::Shaded);
        break;
      }
      case O3deShapeData::Type::SPHERE:
      {
        // gz unit sphere has diameter 1; visual scale gives the diameter.
        auxGeom->DrawSphere(pos, 0.5f * scale.GetX(), color);
        break;
      }
      case O3deShapeData::Type::CYLINDER:
      {
        // gz unit cylinder: diameter 1, length 1 along local +Z.
        auxGeom->DrawCylinder(pos, axisZ, 0.5f * scale.GetX(),
            scale.GetZ(), color);
        break;
      }
      case O3deShapeData::Type::CONE:
      {
        auxGeom->DrawCone(pos, axisZ, 0.5f * scale.GetX(),
            scale.GetZ(), color);
        break;
      }
    }
  }
}

//////////////////////////////////////////////////
O3deBackend &O3deBackend::Instance()
{
  // Leaked on purpose: the O3DE runtime is never torn down (see header).
  static O3deBackend *instance = new O3deBackend();
  return *instance;
}

//////////////////////////////////////////////////
O3deBackend::O3deBackend()
  : dataPtr(new Impl)
{
}

//////////////////////////////////////////////////
O3deBackend::~O3deBackend()
{
  // Intentionally does not stop/destroy the GameApplication.
}

//////////////////////////////////////////////////
bool O3deBackend::IsReady() const
{
  return this->dataPtr->ready;
}

//////////////////////////////////////////////////
bool O3deBackend::Bootstrap()
{
  Impl &d = *this->dataPtr;

  // Start the dedicated O3DE thread once and wait for it to finish bringing up
  // the runtime. All Atom work happens on that thread for the life of the
  // process (see the Impl note: AssetManager only self-pumps loads on the
  // thread that created the application).
  std::unique_lock<std::mutex> lock(d.mutex);
  if (d.renderThread.joinable())
    return d.bootstrapOk;

  d.renderThread = std::thread([&d]() { d.RenderThreadMain(); });
  d.inputCv.notify_all();
  d.outputCv.wait(lock, [&d]() { return d.bootstrapDone; });
  return d.bootstrapOk;
}

//////////////////////////////////////////////////
void O3deBackend::Impl::RenderThreadMain()
{
  const bool ok = this->BootstrapOnThread();
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->bootstrapOk = ok;
    this->ready = ok;
    this->bootstrapDone = true;
  }
  this->outputCv.notify_all();
  if (!ok)
    return;

  // Main render loop. Wait for input (or a short timeout so the engine keeps
  // ticking and the asset/streamer systems stay alive), then render one frame
  // and publish it.
  while (!this->stop.load())
  {
    bool render = false;
    {
      std::unique_lock<std::mutex> lock(this->mutex);
      this->inputCv.wait_for(lock, std::chrono::milliseconds(16),
          [this]() { return this->haveInput || this->stop.load(); });
      if (this->stop.load())
        break;
      if (this->haveInput)
      {
        this->camera = this->pendingCamera;
        this->shapes = this->pendingShapes;
        this->reqWidth = this->pendingWidth;
        this->reqHeight = this->pendingHeight;
        this->haveInput = false;
        render = true;
      }
    }

    if (render)
    {
      this->RenderOneFrame();
    }
    else
    {
      // Idle: keep the engine ticking so async loads/streaming progress.
      this->app->PumpSystemEventLoopUntilEmpty();
      this->app->TickSystem();
      this->app->Tick();
    }
  }
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::BootstrapOnThread()
{
  using FixedValueString = AZ::SettingsRegistryInterface::FixedValueString;

  const char *enginePath = EnvOr("GZ_O3DE_ENGINE_PATH",
      "/home/jrivero/code/gz/gz-rendering/vendor/o3de");
  const char *projectPath = EnvOr("GZ_O3DE_PROJECT_PATH",
      "/home/jrivero/o3de-gzpoc");
  static AZStd::string binPathStorage =
      AZStd::string(EnvOr("GZ_O3DE_BIN_PATH", "")) ;
  if (binPathStorage.empty())
  {
    binPathStorage = AZStd::string(enginePath) + "/build/linux/bin/profile";
  }
  const char *binPath = binPathStorage.c_str();
  const char *projectName = EnvOr("GZ_O3DE_PROJECT_NAME", "GzAtomPoc");

  // Seed the settings registry: engine/project so the manifest, gems and
  // cooked-asset cache are found; project_build_path so the gem .so resolve
  // from the vendored bin folder (the gz host exe lives elsewhere).
  FixedValueString bootstrapJson = R"(
      [
          { "op": "add", "path": "/O3DE", "value": { "Runtime": { "Manifest": { "Project": {} } } } },
          { "op": "add", "path": "/Amazon", "value": { "AzCore": { "Bootstrap": {} } } },)";
  bootstrapJson += FixedValueString::format(R"(
          { "op": "add", "path": "/O3DE/Runtime/Manifest/Project/project_name", "value": "%s" },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/engine_path", "value": "%s" },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/project_path", "value": "%s" },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/project_build_path", "value": "%s" },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/connect_to_remote", "value": 0 },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/wait_for_connect", "value": 0 },
          { "op": "add", "path": "/Amazon/AzCore/Bootstrap/remote_filesystem", "value": 0 }
      ])", projectName, enginePath, projectPath, binPath);

  AZ::ComponentApplicationSettings componentAppSettings;
  componentAppSettings.m_setregBootstrapJson = bootstrapJson;
  componentAppSettings.m_setregFormat =
      AZ::SettingsRegistryInterface::Format::JsonPatch;

  // Fabricate a minimal argv. We are not a real launcher; gz-gui owns the real
  // process arguments, which O3DE must not see.
  static char arg0[] = "gz-rendering-o3de";
  static char arg1[] = "--rhi=vulkan";
  static char *fakeArgv[] = { arg0, arg1, nullptr };
  int fakeArgc = 2;

  this->app = new AzGameFramework::GameApplication(
      fakeArgc, fakeArgv, AZStd::move(componentAppSettings));

  auto *settingsRegistry = AZ::SettingsRegistry::Get();
  if (!settingsRegistry)
  {
    std::fprintf(stderr, "[gz-o3de] no settings registry after ctor\n");
    return false;
  }

  // Load the gem-list setreg by impersonating the GzAtomPoc game launcher
  // build target (reuses its proven Atom gem set + our cooked project).
  AZ::SettingsRegistryMergeUtils::MergeSettingsToRegistry_AddSpecialization(
      *settingsRegistry, "gzatompoc");
  AZ::SettingsRegistryMergeUtils::
      MergeSettingsToRegistry_AddBuildSystemTargetSpecialization(
          *settingsRegistry, "gzatompoc_gamelauncher");

  AzGameFramework::GameApplication::StartupParameters startupParams;
  this->app->Start({}, startupParams);
  std::fprintf(stderr, "[gz-o3de] GameApplication started\n");

  // Let the RHI device + Atom bootstrap settle before building our scene.
  for (int i = 0; i < 15; ++i)
  {
    this->app->PumpSystemEventLoopUntilEmpty();
    this->app->TickSystem();
    this->app->Tick();
  }

  if (!this->SetupScene(512u, 512u))
  {
    std::fprintf(stderr, "[gz-o3de] offscreen scene setup failed\n");
    return false;
  }
  this->outputWidth = 512u;
  this->outputHeight = 512u;

  std::fprintf(stderr, "[gz-o3de] offscreen pipeline ready\n");
  return true;
}

//////////////////////////////////////////////////
bool O3deBackend::RenderFrame(const O3deCameraData &_camera,
    const std::vector<O3deShapeData> &_shapes,
    uint32_t _width, uint32_t _height, uint8_t *_outRgba)
{
  Impl &d = *this->dataPtr;
  if (!d.ready || !_outRgba || _width == 0u || _height == 0u)
    return false;

  // Assemble the shape list on the caller's thread (only plain data crosses to
  // the render thread). Demo aid: gz-gui's MinimalScene starts with an empty
  // scene (primitives normally come from gz-sim); when GZ_O3DE_DEMO_SHAPES is
  // set and there are no primitives, inject a box/sphere/cylinder trio so the
  // live viewer has something to show.
  std::vector<O3deShapeData> shapes = _shapes;
  if (shapes.empty() && std::getenv("GZ_O3DE_DEMO_SHAPES"))
  {
    O3deShapeData box;
    box.type = O3deShapeData::Type::BOX;
    box.pos[0] = 0.0; box.pos[1] = 1.5; box.pos[2] = 0.5;
    box.color[0] = 1.0f; box.color[1] = 0.0f; box.color[2] = 0.0f;
    shapes.push_back(box);

    O3deShapeData sphere;
    sphere.type = O3deShapeData::Type::SPHERE;
    sphere.pos[0] = 0.0; sphere.pos[1] = 0.0; sphere.pos[2] = 0.5;
    sphere.color[0] = 0.0f; sphere.color[1] = 1.0f; sphere.color[2] = 0.0f;
    shapes.push_back(sphere);

    O3deShapeData cylinder;
    cylinder.type = O3deShapeData::Type::CYLINDER;
    cylinder.pos[0] = 0.0; cylinder.pos[1] = -1.5; cylinder.pos[2] = 0.5;
    cylinder.scale[2] = 1.5;
    cylinder.color[0] = 0.0f; cylinder.color[1] = 0.0f; cylinder.color[2] = 1.0f;
    shapes.push_back(cylinder);
  }

  // Post the input to the render thread and wait for a frame rendered after it.
  uint64_t startSeq = 0u;
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.pendingCamera = _camera;
    d.pendingShapes = std::move(shapes);
    d.pendingWidth = _width;
    d.pendingHeight = _height;
    d.haveInput = true;
    startSeq = d.frameSeq;
  }
  d.inputCv.notify_all();

  std::unique_lock<std::mutex> lock(d.mutex);
  // Bounded wait so we never hang gz-gui's render thread if a frame stalls.
  const bool got = d.outputCv.wait_for(lock, std::chrono::seconds(5),
      [&d, startSeq]()
      { return d.frameSeq > startSeq && !d.latestFrame.empty(); });
  if (!got || d.latestFrame.empty())
    return false;

  // Copy the latest RGBA8888 frame into the caller's buffer, clamping to the
  // overlap (we render at a locked resolution; the caller's image may differ).
  const uint32_t cw = d.latestWidth;
  const uint32_t ch = d.latestHeight;
  const uint32_t copyW = (cw < _width) ? cw : _width;
  const uint32_t copyH = (ch < _height) ? ch : _height;
  std::memset(_outRgba, 0, static_cast<size_t>(_width) * _height * 4u);
  for (uint32_t y = 0; y < copyH; ++y)
  {
    const uint8_t *src = d.latestFrame.data() +
        static_cast<size_t>(y) * cw * 4u;
    uint8_t *dst = _outRgba + static_cast<size_t>(y) * _width * 4u;
    std::memcpy(dst, src, static_cast<size_t>(copyW) * 4u);
  }
  return true;
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::RenderOneFrame()
{
  static int frameNum = 0;
  ++frameNum;
  // Log the first few frames in full, then every 20th, to characterise the
  // live frame rate without spamming.
  const bool logThis = (frameNum <= 3) || (frameNum % 20 == 0);
  const auto frameStart = std::chrono::steady_clock::now();

  // Size the offscreen target to the requested resolution whenever it changes
  // (e.g. gz-gui's window is resized). A resize recreates the pass attachments,
  // so render a few warm-up ticks at the new size before requesting a capture
  // (capturing an attachment that has not yet been rendered at the new size
  // never completes). Resizing is safe here because all O3DE work runs on this
  // one thread: the synchronous shader loads a pass rebuild can trigger
  // self-pump and complete (from a foreign thread they would deadlock).
  const uint32_t w = (this->reqWidth > 0u) ? this->reqWidth : this->outputWidth;
  const uint32_t h =
      (this->reqHeight > 0u) ? this->reqHeight : this->outputHeight;
  if (w > 0u && (w != this->outputWidth || h != this->outputHeight))
  {
    this->EnsureSize(w, h);
    this->outputWidth = w;
    this->outputHeight = h;

    this->ApplyCamera(w, h);
    this->pipeline->AddToRenderTick();
    for (int k = 0; k < 3; ++k)
    {
      this->SubmitPrimitives();
      this->app->PumpSystemEventLoopUntilEmpty();
      this->app->TickSystem();
      this->app->Tick();
    }
    this->pipeline->RemoveFromRenderTick();
  }

  const uint32_t rw = this->outputWidth;
  const uint32_t rh = this->outputHeight;

  // Point the view at this frame's camera pose + projection (locked aspect).
  this->ApplyCamera(rw, rh);

  this->captureDone = false;
  this->captureBuffer.clear();
  this->captureWidth = 0u;
  this->captureHeight = 0u;

  Impl *self = this;
  auto captureCallback =
      [self](const AZ::RPI::AttachmentReadback::ReadbackResult &result)
  {
    if (result.m_dataBuffer && !result.m_dataBuffer->empty())
    {
      self->captureWidth = result.m_imageDescriptor.m_size.m_width;
      self->captureHeight = result.m_imageDescriptor.m_size.m_height;
      self->captureBuffer = *result.m_dataBuffer;
    }
    self->captureDone = true;
  };

  // A *fresh* add before each capture is required: capturing while the pipeline
  // has been continuously in the render tick since a previous capture deadlocks
  // the RHI. Toggling it forces the frame-graph to rebuild cleanly per readback.
  this->pipeline->AddToRenderTick();

  AZStd::vector<AZStd::string> passHierarchy = { this->pipelineName };
  AZ::Render::FrameCaptureOutcome captureOutcome;
  AZ::Render::FrameCaptureRequestBus::BroadcastResult(
      captureOutcome,
      &AZ::Render::FrameCaptureRequestBus::Events::
          CapturePassAttachmentWithCallback,
      captureCallback,
      passHierarchy,
      AZStd::string("Output"),
      AZ::RPI::PassAttachmentReadbackOption::Output);

  if (!captureOutcome.IsSuccess())
  {
    std::fprintf(stderr, "[gz-o3de] capture request failed: %s\n",
        captureOutcome.GetError().m_errorMessage.c_str());
    this->pipeline->RemoveFromRenderTick();
    return false;
  }

  // Wait for the capture to finish *fully* (readback done AND recycled to the
  // idle pool). All ticking is on this thread, so synchronous asset loads
  // triggered by a pass rebuild self-pump and complete (the whole reason for
  // this dedicated thread).
  CaptureFinishedListener finishListener;
  finishListener.BusConnect(captureOutcome.GetValue());

  const int maxFrames = 30;
  int iters = 0;
  for (; iters < maxFrames && !finishListener.finished; ++iters)
  {
    this->SubmitPrimitives();
    this->app->PumpSystemEventLoopUntilEmpty();
    this->app->TickSystem();
    this->app->Tick();
  }

  this->pipeline->RemoveFromRenderTick();

  if (logThis)
  {
    const auto frameMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - frameStart).count();
    std::fprintf(stderr,
        "[gz-o3de] frame %d: captureDone=%d after %d ticks, "
        "buffer=%zu (%ux%u), %lld ms\n",
        frameNum, this->captureDone ? 1 : 0, iters, this->captureBuffer.size(),
        this->captureWidth, this->captureHeight,
        static_cast<long long>(frameMs));
  }

  if (!this->captureDone || this->captureBuffer.empty())
  {
    std::fprintf(stderr, "[gz-o3de] capture did not complete\n");
    return false;
  }

  // Publish the completed frame for RenderFrame() to pick up.
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->latestFrame.assign(
        this->captureBuffer.begin(), this->captureBuffer.end());
    this->latestWidth = this->captureWidth;
    this->latestHeight = this->captureHeight;
    ++this->frameSeq;
  }
  this->outputCv.notify_all();
  return true;
}

}  // namespace rendering
}  // namespace gz
