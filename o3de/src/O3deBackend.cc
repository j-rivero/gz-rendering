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

// M4 interop (GZ_O3DE_INTEROP): runtime resolution of the Vulkan loader entry
// points and FD ownership for the export proof.
#include <dlfcn.h>
#include <unistd.h>

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
#include <Atom/RPI.Public/Image/AttachmentImage.h>
#include <Atom/RPI.Public/Image/AttachmentImagePool.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Reflect/Image/AttachmentImageAssetCreator.h>
#include <Atom/RPI.Reflect/System/SceneDescriptor.h>
#include <Atom/RPI.Reflect/System/RenderPipelineDescriptor.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/Image.h>
#include <Atom/RHI/ImagePool.h>
#include <Atom/RHI/DeviceImage.h>
#include <Atom/RHI/Fence.h>
#include <Atom/RHI/DeviceFence.h>
#include <Atom/RHI/ScopeProducerFunction.h>
#include <Atom/RHI/FrameGraphInterface.h>
#include <Atom/RHI/FrameGraphBuilder.h>
#include <Atom/RHI/FrameGraphCompileContext.h>
#include <Atom/RHI/FrameGraphExecuteContext.h>
#include <Atom/RHI.Reflect/ImageSubresource.h>
#include <Atom/RHI.Reflect/MultisampleState.h>
#include <Atom/Feature/Utils/FrameCaptureBus.h>

#include <AzFramework/Scene/Scene.h>
#include <AzFramework/Scene/SceneSystemInterface.h>

// M4 zero-copy interop: the Vulkan RHI's external-handle bus. Header-only EBus
// (the bus context is shared across modules via the AZ environment), so no link
// against the Vulkan RHI gem is needed just to connect a handler. <vulkan/vulkan.h>
// supplies the Vk* flag types VulkanBus.h refers to but does not itself include.
#include <vulkan/vulkan.h>
#include <Atom/RHI.Reflect/Vulkan/VulkanBus.h>
#if defined(GZ_O3DE_INTEROP_BUILD)
// Native-handle accessors (VkDevice / VkImage / VkDeviceMemory) exported from the
// loaded Vulkan RHI gem .so by the gz-rendering interop patch
// (o3de/patches/0001-export-vulkan-native-handle-accessors.patch). Pulls only the
// public Atom/RHI headers + <vulkan/vulkan.h>, no glad/internal Source headers.
// Only included for interop builds (-DGZ_O3DE_INTEROP=ON), which require the patch.
#include <Atom/RHI.Interface/Vulkan/RHIVulkanInterface.h>
#endif

#include "O3deBackend.hh"
#include "O3deGlInterop.hh"  // plain-types declaration; no GL headers leak here
#include "O3deVkInterop.hh"  // plain-types declaration; no Vulkan headers leak here

namespace gz
{
namespace rendering
{

namespace
{
  // Default supersampling factor for anti-aliasing. The offscreen target is
  // rendered at this multiple of the requested (logical) resolution in each
  // dimension and box-downsampled on readback. Atom draws AuxGeom (our
  // primitives) as a post-resolve overlay at 1 sample, so pipeline MSAA cannot
  // antialias it; supersampling smooths it (and everything else) on the
  // CPU-readback path. 2 => 4x the pixels rendered (acceptable for this readback
  // PoC). Overridable at runtime via GZ_O3DE_SSAA (1 = off ... 4); see
  // Impl::ssaaScale.
  constexpr uint32_t kDefaultSsaaScale = 2u;
  constexpr uint32_t kMaxSsaaScale = 4u;

  // PoC paths. We reuse the vendored O3DE build and the GzAtomPoc cooked-asset
  // project that M1/M2.1 produced. A later milestone will vendor a minimal
  // asset bundle and derive these from the gz install layout. All are
  // overridable via environment variables for portability.
  const char *EnvOr(const char *_envName, const char *_fallback)
  {
    const char *value = std::getenv(_envName);
    return (value && value[0]) ? value : _fallback;
  }

  // M4 interop foundation: connected before the RHI device is created, this
  // handler makes Atom allocate every image's backing memory and every timeline
  // semaphore as exportable (OPAQUE_FD). The Vulkan RHI queries this same bus at
  // device creation (VMA pTypeExternalMemoryHandleTypes), image creation
  // (VkExternalMemoryImageCreateInfo) and timeline-semaphore creation
  // (VkExportSemaphoreCreateInfo) -- see Device.cpp / TimelineSemaphoreFence.cpp.
  // With it connected, the offscreen render-target image can be exported as an
  // FD and imported into GL (the zero-copy display path); see M4_INTEROP_DESIGN.md.
  // The logs confirm Atom actually queried the bus (proving the resources are
  // created exportable). Gated by GZ_O3DE_INTEROP so the default readback path is
  // completely unaffected.
  //
  // It is also a DeviceRequirementBus handler: making memory *exportable* (above)
  // is necessary but not sufficient to call vkGetMemoryFdKHR -- that entry point
  // requires the VK_KHR_external_memory_fd *device extension* to be enabled at
  // device creation, and O3DE does not enable it on its own (it only uses the
  // semaphore-FD extension). Device::GetRequiredExtensions() broadcasts this bus,
  // so we add the extension here. Required only when interop is on.
  class GzExternalHandleProvider
      : public AZ::Vulkan::ExternalHandleRequirementBus::Handler
      , public AZ::Vulkan::DeviceRequirementBus::Handler
  {
    public: void CollectExternalMemoryRequirements(
        VkExternalMemoryHandleTypeFlagsKHR &_flags) override
    {
      _flags |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
      if (!this->loggedMemory)
      {
        std::fprintf(stderr,
            "[gz-o3de] interop: Atom queried external-memory requirements "
            "-> requesting OPAQUE_FD (images become exportable)\n");
        this->loggedMemory = true;
      }
    }

    public: void CollectSemaphoreExportHandleTypes(
        VkExternalSemaphoreHandleTypeFlags &_flags) override
    {
      _flags |= VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
      if (!this->loggedSemaphore)
      {
        std::fprintf(stderr,
            "[gz-o3de] interop: Atom queried semaphore-export requirements "
            "-> requesting OPAQUE_FD (timeline semaphores become exportable)\n");
        this->loggedSemaphore = true;
      }
    }

    // DeviceRequirementBus: enable the external-memory FD device extension so
    // vkGetMemoryFdKHR is available. VK_KHR_external_memory is core since Vulkan
    // 1.1 but NVIDIA still advertises it as a device extension; enabling it is
    // harmless and satisfies the _fd extension's dependency on older drivers.
    public: void CollectAdditionalRequiredDeviceExtensions(
        AZStd::vector<AZStd::string> &_extensions) override
    {
      _extensions.emplace_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
      _extensions.emplace_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
      if (!this->loggedExtensions)
      {
        std::fprintf(stderr,
            "[gz-o3de] interop: requesting device extensions %s + %s "
            "(needed for vkGetMemoryFdKHR)\n",
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        this->loggedExtensions = true;
      }
    }

    private: bool loggedMemory = false;
    private: bool loggedSemaphore = false;
    private: bool loggedExtensions = false;
  };
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
///
/// Inherits RHISystemNotificationBus::Handler for the #26 producer-side render-
/// finished signal: OnFramePrepare() fires every RHI frame (after RPI pass
/// registration) and imports a fence-signal scope on the flagged render tick.
/// The handler is connected only on the interop-live path; off it, the bus is
/// never connected, so the base class is inert.
class O3deBackend::Impl
    : public AZ::RHI::RHISystemNotificationBus::Handler
{
  public: AzGameFramework::GameApplication *app = nullptr;
  public: AZ::RPI::ScenePtr scene;
  public: AZStd::shared_ptr<AzFramework::Scene> frameworkScene;
  public: AZ::RPI::RenderPipelinePtr pipeline;
  public: AZ::RPI::ViewPtr view;

  public: const AZStd::string pipelineName = "GzO3deProbePipeline";
  public: uint32_t outputWidth = 0u;
  public: uint32_t outputHeight = 0u;

  // SSAA factor (>=1). Set once from GZ_O3DE_SSAA during bootstrap; 1 disables
  // supersampling. outputWidth/Height track the supersampled render size.
  public: uint32_t ssaaScale = kDefaultSsaaScale;

  // M4 interop (experimental, GZ_O3DE_INTEROP): a persistent, exportable colour
  // image we own. Persistent (unlike the pass's transient RTT output) so its
  // VkImage/VkDeviceMemory are stable and shareable; created after the bus
  // handler is connected so its VMA memory is allocated exportable. Kept alive
  // here for reuse by later M4 steps (render-into + GL import). Null when interop
  // is off.
  public: AZ::Data::Instance<AZ::RPI::AttachmentImage> interopImage;
  // The AttachmentImageAsset backing interopImage. Kept so the Stage B live
  // pipeline (CreateRenderPipelineForImage) can bind the image as its output;
  // built by EnsureInteropImage via AttachmentImageAssetCreator.
  public: AZ::Data::Asset<AZ::RPI::AttachmentImageAsset> interopImageAsset;
  public: bool interopFdProven = false;
  // Stored import handles for the exportable interop image (M4 step 2). The FD
  // is kept open (dup'd to callers via GetInteropImport) so a GL-context thread
  // can import the image without any O3DE call. interopImageReady gates access.
  public: bool interopImageReady = false;
  public: int interopFd = -1;
  public: uint32_t interopWidth = 0u;
  public: uint32_t interopHeight = 0u;
  public: uint64_t interopAllocSize = 0u;
  public: uint64_t interopAllocOffset = 0u;
  // Bumped on each (re)creation of the exportable image (probe -> camera size,
  // resize). Consumers compare it (via O3deInteropImport::generation) to decide
  // when to re-import. Retired images are kept alive (not freed) so any FD a
  // consumer already imported from a previous generation stays backed.
  public: uint64_t interopGeneration = 0u;
  public: std::vector<AZ::Data::Instance<AZ::RPI::AttachmentImage>>
      retiredInteropImages;

  // M4 interop, Stage B (#26): render-finished synchronisation. interopSemaphoreFd
  // is the exported OPAQUE_FD of a render-finished TIMELINE semaphore (Atom's
  // TimelineSemaphoreFence); interopSemaphoreValue is the per-frame value the
  // consumer waits on. The producer signals it each frame via the fence-signal
  // scope below (no custom RPI::Pass needed -- RHISystemNotificationBus +
  // ImportScopeProducer + FrameGraphInterface::SignalFence). Plumbed through
  // GetInteropImport(), gated by interopSemaphoreReady (GZ_O3DE_INTEROP_SEM).
  // VERIFIED working (monotonic shared counter), but it does NOT fix the live
  // device loss: that is a separate cross-device render-target/compression handoff
  // problem (see o3de/docs/zero-copy-interop-findings.md).
  public: AZ::RHI::Ptr<AZ::RHI::Fence> renderFinishedFence;
  public: int interopSemaphoreFd = -1;
  public: uint64_t interopSemaphoreValue = 0u;
  // #26 producer signal path. fenceSignalScope is a standalone scope, imported
  // via OnFramePrepare() on the one render tick per frame flagged by
  // signalFenceThisTick, that copy-reads the interop image (read-after-write, so
  // the scheduler orders it after the pipeline's write) and signals
  // renderFinishedFence -- the fence therefore signals only once the frame's
  // render into the shared image is GPU-complete. interopSemaphoreReady gates
  // advertising the exported FD to the consumer (env GZ_O3DE_INTEROP_SEM): off ->
  // the producer signals + the per-frame value is logged but the consumer does
  // not yet wait on it (Step A bring-up); on -> the consumer waits (Step B).
  public: AZStd::shared_ptr<AZ::RHI::ScopeProducer> fenceSignalScope;
  public: bool signalFenceThisTick = false;
  public: bool interopSemaphoreReady = false;

  // M4 interop (experimental, GZ_O3DE_INTEROP): when set, this handler is
  // connected before the RHI device is created so Atom makes images + semaphores
  // exportable. Member of the (leaked) Impl so it stays connected for the life of
  // the runtime. Off by default -> zero effect on the readback path.
  public: bool interop = false;

  // M4 interop, Phase 2a (experimental, GZ_O3DE_INTEROP_LIVE): when set *and*
  // interop is on, RenderOneFrame() (re)creates the exportable image at the live
  // camera size each frame and uploads the freshly rendered scene into it, so a
  // native Vulkan consumer samples the live scene instead of the static probe.
  // Off by default: the live path needs cross-device render-finished-semaphore
  // sync (task #26) to be safe; without it the producer's per-frame writes race
  // the consumer's unmatched EXTERNAL ownership acquire and the GPU device is
  // lost. The default interop run therefore stays on the stable static probe.
  public: bool interopLive = false;
  public: GzExternalHandleProvider externalHandleProvider;

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

  /// \brief M4 step 1b: prove that an Atom-created image exports an OS handle.
  /// Creates a persistent, exportable AttachmentImage, pulls its native
  /// VkDeviceMemory via the patched Vulkan-RHI accessors, and calls
  /// vkGetMemoryFdKHR. A returned FD >= 0 proves the GZ_O3DE_INTEROP bus handler
  /// made Atom's VMA-backed image memory exportable -- the foundation for the
  /// zero-copy Vulkan->GL path. Runs once, only when interop is enabled; never
  /// touches the readback path. Must run on the render thread (all O3DE work).
  public: void ProveFdExportOnce();
  /// \brief (Re)create the persistent, exportable colour image at the given
  /// size and export its OPAQUE_FD. Idempotent when the size is unchanged;
  /// otherwise bumps interopGeneration and retires the old image. Render thread
  /// only. \return True on a ready exportable image.
  public: bool EnsureInteropImage(uint32_t _w, uint32_t _h);
  /// \brief Upload a tightly packed RGBA8888 frame into the current exportable
  /// image (caller must EnsureInteropImage() first). Render thread only.
  public: void UploadToInteropImage(const uint8_t *_rgba, uint32_t _w,
              uint32_t _h);
  /// \brief Stage B (#26): create the render-finished TIMELINE fence once
  /// (usedForWaitingOnDevice=true selects TimelineSemaphoreFence) and export its
  /// native VkSemaphore as an OPAQUE_FD. Idempotent. Render thread only. \return
  /// True once the fence + exported semaphore FD are ready. The per-frame GPU
  /// signal is wired via EnsureFenceSignalScope() + the RHISystemNotificationBus
  /// handler; the FD is advertised to the consumer only when interopSemaphoreReady
  /// (GZ_O3DE_INTEROP_SEM) is set.
  public: bool EnsureInteropSemaphore();
  /// \brief #26 producer signal: build the fence-signal scope once (idempotent).
  /// Needs renderFinishedFence + the interop image to exist. Render thread only.
  /// \return True once fenceSignalScope is ready.
  public: bool EnsureFenceSignalScope();
  /// \brief #26 scope Prepare callback: copy-read the interop image (so the
  /// scheduler orders this scope read-after-write of the pipeline's render) and
  /// signal renderFinishedFence. Bound into the ScopeProducerFunctionNoData.
  public: void FenceSignalPrepare(AZ::RHI::FrameGraphInterface _frameGraph);
  /// \brief RHISystemNotificationBus: import the fence-signal scope on the render
  /// tick flagged by signalFenceThisTick. Fires every app Tick (after RPI passes
  /// register), so the flag keeps the import to exactly one tick per frame.
  public: void OnFramePrepare(AZ::RHI::FrameGraphBuilder &_builder) override;
  /// \brief Stage B live path: (re)create the exportable image at \p _w x \p _h
  /// and a render pipeline (CreateRenderPipelineForImage) that renders the scene
  /// directly into it -- zero-copy, no CPU readback. Swaps out the previous
  /// pipeline on a size change. Render thread only. \return True on success.
  public: bool RecreateInteropPipeline(uint32_t _w, uint32_t _h);
  /// \brief (Re)size the offscreen render target if needed.
  public: void EnsureSize(uint32_t _width, uint32_t _height);
  /// \brief Point the RPI view at the current gz camera pose + projection.
  public: void ApplyCamera(uint32_t _width, uint32_t _height);
  /// \brief Submit the current frame's AuxGeom primitives.
  public: void SubmitPrimitives();
  /// \brief Stop driving the offscreen pipeline when the render thread is told
  /// to stop. Runs on the render thread itself (process exit). It deliberately
  /// does NOT tear down the O3DE/Vulkan runtime -- that crashes in upstream GPU
  /// teardown; the runtime is leaked and the process quick_exit()s instead (see
  /// the Bootstrap() exit-handler note).
  public: void TeardownOnThread();
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

#if defined(GZ_O3DE_INTEROP_BUILD)
  if (this->interopLive)
  {
    // Stage B: render the scene DIRECTLY into the exportable image (zero-copy).
    // Create the view + register the scene, then a pipeline whose output IS the
    // exportable image (CreateRenderPipelineForImage). No MainPipelineRenderTo-
    // Texture pipeline and no CPU capture/readback on this path -- the consumer
    // samples the image the scene rendered into.
    this->view = AZ::RPI::View::CreateView(
        AZ::Name("MainCamera"), AZ::RPI::View::UsageCamera);
    this->scene->Activate();
    AZ::RPI::RPISystemInterface::Get()->RegisterScene(this->scene);
    // Create the exportable image + pipeline at the bootstrap placeholder size so
    // the consumer has an image to import the moment it builds its texture node
    // (deferring until the first frame races Qt, which then presents a null
    // VkImage and crashes). The first real frame recreates it at the camera size;
    // that resize is made safe by the consumer retiring (not immediately freeing)
    // old imports -- the earlier device loss was Qt sampling an import we freed
    // out from under its in-flight frame.
    if (!this->RecreateInteropPipeline(_width, _height))
      return false;
    // #26: create the render-finished fence + the scope that signals it after the
    // per-frame render-into, then connect the RHISystem bus so the scope is
    // imported each rendered frame (OnFramePrepare). interopSemaphoreReady --
    // env-gated by GZ_O3DE_INTEROP_SEM -- decides whether the exported semaphore
    // FD is advertised to the consumer. Failure here is non-fatal: the live path
    // still renders (host-sync), just without the cross-device fence.
    if (this->EnsureInteropSemaphore() && this->EnsureFenceSignalScope())
    {
      this->interopSemaphoreReady =
          (std::getenv("GZ_O3DE_INTEROP_SEM") != nullptr);
      AZ::RHI::RHISystemNotificationBus::Handler::BusConnect();
      std::fprintf(stderr,
          "[gz-o3de] interop #26: fence-signal scope wired (consumer wait %s)\n",
          this->interopSemaphoreReady ? "ON" : "OFF (set GZ_O3DE_INTEROP_SEM)");
    }
    else
    {
      std::fprintf(stderr,
          "[gz-o3de] interop #26: fence-signal scope NOT wired (fence/scope "
          "setup failed); live path runs without cross-device sync\n");
    }
    this->ApplyCamera(_width, _height);
    // Settle the pipeline's pass + shader-variant builds (they self-pump on this
    // thread). AuxGeom is submitted so the forward pass has something to draw.
    this->pipeline->AddToRenderTick();
    for (int i = 0; i < 5; ++i)
    {
      this->SubmitPrimitives();
      this->app->PumpSystemEventLoopUntilEmpty();
      this->app->TickSystem();
      this->app->Tick();
    }
    this->pipeline->RemoveFromRenderTick();
    std::fprintf(stderr, "[gz-o3de] interop: Stage B scene setup complete\n");
    return true;
  }
#endif

  // Offscreen render-to-texture pipeline (no window/swapchain).
  // MainPipelineRenderToTexture is the game pipeline's offscreen variant;
  // ToolsPipelineRenderToTexture pulls in editor-only passes whose templates
  // are not cooked in this project.
  AZ::RPI::RenderPipelineDescriptor pipelineDesc;
  pipelineDesc.m_mainViewTagName = "MainCamera";
  pipelineDesc.m_name = this->pipelineName;
  pipelineDesc.m_rootPassTemplate = "MainPipelineRenderToTexture";

  // Enable 4x MSAA. MainPipeline renders to multisampled targets and resolves
  // to the single-sample "Output" attachment we read back, so anti-aliasing is
  // applied transparently to the CPU-readback path. The sample count is set on
  // the descriptor so the pipeline's render targets are created multisampled.
  const AZ::RHI::MultisampleState msaaState(
      /*samples*/ static_cast<uint16_t>(4), /*quality*/ static_cast<uint16_t>(0));
  pipelineDesc.m_renderSettings.m_multisampleState = msaaState;

  this->pipeline = AZ::RPI::RenderPipeline::CreateRenderPipeline(pipelineDesc);
  this->scene->AddRenderPipeline(this->pipeline);
  this->scene->Activate();
  rpiSystem->RegisterScene(this->scene);

  // Apply the multisample state at the application level *after* the scene is
  // registered (mirrors BootstrapSystemComponent). This both selects the MSAA
  // shader supervariant and walks the registered scenes' pipelines marking
  // their passes for rebuild with that supervariant -- the step that actually
  // makes the rasterization multisampled. Calling it before RegisterScene (when
  // m_scenes is empty) is a no-op for the pipeline and leaves it at 1x.
  rpiSystem->SetApplicationMultisampleState(
      this->pipeline->GetRenderSettings().m_multisampleState);

  // Create the view; pose + projection are set per-frame by ApplyCamera().
  this->view = AZ::RPI::View::CreateView(
      AZ::Name("MainCamera"), AZ::RPI::View::UsageCamera);
  this->pipeline->SetDefaultView(this->view);
  this->ApplyCamera(_width, _height);

  // Flush the one-time MSAA pass rebuild here, during setup. SetApplication-
  // MultisampleState() above only *marks* the pipeline's passes for rebuild; the
  // rebuild (which recreates the render-to-texture pass and resets its output
  // size to the template default) actually runs on the next render ticks. If we
  // let it happen later -- on the first RenderOneFrame() -- it clobbers the
  // EnsureSize() that frame uses to size the target to the window, and we end up
  // capturing at the template size. So tick a few times now to let the rebuild
  // (and its MSAA shader-variant loads, which self-pump on this thread) settle,
  // then size the target. All ticking is on this dedicated thread.
  this->pipeline->AddToRenderTick();
  this->EnsureSize(_width, _height);
  for (int i = 0; i < 3; ++i)
  {
    this->app->PumpSystemEventLoopUntilEmpty();
    this->app->TickSystem();
    this->app->Tick();
  }

  // Re-assert the size after the rebuild, then keep the pipeline out of the
  // per-tick render loop until we explicitly request a frame (default
  // RenderEveryTick would render an unsized RTT target and crash the RHI).
  this->EnsureSize(_width, _height);
  this->pipeline->RemoveFromRenderTick();

  // M4 step 1b: with the device + image system now up, prove the exportable-image
  // foundation. No-op unless GZ_O3DE_INTEROP is set; the readback path is
  // untouched either way.
  if (this->interop)
  {
    this->ProveFdExportOnce();

    // M4 step 2b: optional headless GL self-test of the zero-copy import path.
    // Off by default (GZ_O3DE_INTEROP_GLTEST) -- it creates its own GL context,
    // imports the exported image, and verifies the gradient round-trips. Runs
    // here (render thread, post-bootstrap) so the GL context lifetime is local.
    if (std::getenv("GZ_O3DE_INTEROP_GLTEST"))
      RunO3deInteropGlSelfTest();

    // M4 "Strategy 2": optional headless Vulkan->Vulkan self-test. Off by default
    // (GZ_O3DE_INTEROP_VKTEST) -- it creates its own VkInstance/VkDevice (the
    // Qt-device stand-in), imports the exported FD via VkImportMemoryFdInfoKHR,
    // and verifies the gradient round-trips. This is the path gz-gui's
    // MinimalSceneRhiVulkan would use on Qt's QRhi device.
    if (std::getenv("GZ_O3DE_INTEROP_VKTEST"))
      RunO3deInteropVkSelfTest();
  }

  return true;
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::EnsureInteropImage(uint32_t _w, uint32_t _h)
{
#if !defined(GZ_O3DE_INTEROP_BUILD)
  (void)_w; (void)_h;
  return false;
#else
  if (_w == 0u || _h == 0u)
    return false;
  if (this->interopImage && this->interopImageReady &&
      this->interopWidth == _w && this->interopHeight == _h)
    return true;  // already the right size; reuse

  // A persistent colour image from the system attachment pool. Persistent (not
  // transient) so its backing VkImage/VkDeviceMemory are stable; created after
  // GzExternalHandleProvider connected pre-device-creation, so its VMA
  // allocation carries VkExportMemoryAllocateInfo (OPAQUE_FD).
  const auto *imageSystem = AZ::RPI::ImageSystemInterface::Get();
  if (!imageSystem || !imageSystem->GetSystemAttachmentPool())
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: no system attachment pool; cannot create image\n");
    return false;
  }
  // Color (render-into) + ShaderRead (consumer sampling) + CopyRead (readback) +
  // CopyWrite (UpdateImageContents upload).
  const AZ::RHI::ImageDescriptor desc = AZ::RHI::ImageDescriptor::Create2D(
      AZ::RHI::ImageBindFlags::Color | AZ::RHI::ImageBindFlags::ShaderRead |
          AZ::RHI::ImageBindFlags::CopyRead | AZ::RHI::ImageBindFlags::CopyWrite,
      _w, _h, AZ::RHI::Format::R8G8B8A8_UNORM);
  // AttachmentImage names must be unique per live instance; suffix the next
  // generation so a recreate (resize) does not collide with the retired one.
  const AZStd::string imgName = AZStd::string::format(
      "GzInteropImage_%llu",
      static_cast<unsigned long long>(this->interopGeneration + 1u));

  // Build the image through an AttachmentImageAsset (mirrors what
  // AttachmentImage::Create does internally) so we keep the asset handle: the
  // Stage B live pipeline binds the image as its render output by asset id via
  // CreateRenderPipelineForImage. A random asset id keys a fresh instance each
  // generation; End() registers it with the AssetManager and defaults to the
  // system attachment pool.
  const AZ::Data::AssetId assetId(AZ::Uuid::CreateRandom());
  AZ::RPI::AttachmentImageAssetCreator creator;
  creator.Begin(assetId);
  creator.SetImageDescriptor(desc);
  creator.SetName(AZ::Name(imgName), /*isUniqueName*/ true);
  AZ::Data::Asset<AZ::RPI::AttachmentImageAsset> imageAsset;
  if (!creator.End(imageAsset) || !imageAsset.IsReady())
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: AttachmentImageAsset build failed (%ux%u)\n", _w, _h);
    return false;
  }
  AZ::Data::Instance<AZ::RPI::AttachmentImage> newImage =
      AZ::RPI::AttachmentImage::FindOrCreate(imageAsset);
  if (!newImage || !newImage->GetRHIImage())
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: AttachmentImage::FindOrCreate failed (%ux%u)\n",
        _w, _h);
    return false;
  }

  // Atom RHI handles -> per-device objects -> native Vulkan handles (via the
  // patched, now-exported gem accessors).
  const int deviceIndex = AZ::RHI::MultiDevice::DefaultDeviceIndex;
  AZ::RHI::Image *rhiImage = newImage->GetRHIImage();
  AZ::RHI::Ptr<AZ::RHI::DeviceImage> deviceImage =
      rhiImage->GetDeviceImage(deviceIndex);
  AZ::RHI::Device *device =
      AZ::RHI::RHISystemInterface::Get()->GetDevice(deviceIndex);
  if (!deviceImage || !device)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: no device image/device at index %d\n", deviceIndex);
    return false;
  }
  const VkDevice vkDevice = AZ::Vulkan::GetDeviceNativeHandle(*device);
  const VkDeviceMemory vkMemory = AZ::Vulkan::GetImageMemory(*deviceImage);
  const VkImage vkImage = AZ::Vulkan::GetNativeImage(*deviceImage);
  const size_t allocSize = AZ::Vulkan::GetImageAllocationSize(*deviceImage);
  const size_t allocOffset = AZ::Vulkan::GetImageAllocationOffset(*deviceImage);
  if (vkDevice == VK_NULL_HANDLE || vkMemory == VK_NULL_HANDLE)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: null VkDevice/VkDeviceMemory from gem accessors\n");
    return false;
  }

  // vkGetMemoryFdKHR is a device extension entry point. O3DE already dlopen'd
  // the Vulkan loader (via glad), so resolve it through the loaded loader.
  void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
  auto getDeviceProcAddr = loader
      ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            dlsym(loader, "vkGetDeviceProcAddr"))
      : nullptr;
  auto getMemoryFd = getDeviceProcAddr
      ? reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            getDeviceProcAddr(vkDevice, "vkGetMemoryFdKHR"))
      : nullptr;
  if (!getMemoryFd)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: could not resolve vkGetMemoryFdKHR (loader=%p)\n",
        loader);
    return false;
  }
  VkMemoryGetFdInfoKHR getFdInfo{};
  getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  getFdInfo.memory = vkMemory;
  getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
  int fd = -1;
  const VkResult res = getMemoryFd(vkDevice, &getFdInfo, &fd);
  if (res != VK_SUCCESS || fd < 0)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: vkGetMemoryFdKHR FAILED (VkResult=%d, fd=%d) -- the "
        "image memory is not exportable; check the bus handler / device ext\n",
        static_cast<int>(res), fd);
    return false;
  }

  // Publish the new image atomically for GetInteropImport() (gz-gui thread).
  // Retire (keep alive, do not free) the previous image so any FD a consumer
  // already imported from it stays backed; the bumped generation makes the
  // consumer re-import the new one.
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->interopImage)
      this->retiredInteropImages.push_back(this->interopImage);
    if (this->interopFd >= 0)
      ::close(this->interopFd);
    this->interopImage = newImage;
    this->interopImageAsset = imageAsset;
    this->interopFd = fd;
    this->interopWidth = _w;
    this->interopHeight = _h;
    this->interopAllocSize = static_cast<uint64_t>(allocSize);
    this->interopAllocOffset = static_cast<uint64_t>(allocOffset);
    this->interopImageReady = true;
    ++this->interopGeneration;
  }
  std::fprintf(stderr,
      "[gz-o3de] interop: exportable image ready gen=%llu %ux%u fd=%d "
      "VkImage=%p (alloc size=%zu offset=%zu)\n",
      static_cast<unsigned long long>(this->interopGeneration), _w, _h, fd,
      reinterpret_cast<void *>(vkImage), allocSize, allocOffset);
  return true;
#endif  // GZ_O3DE_INTEROP_BUILD
}

//////////////////////////////////////////////////
void O3deBackend::Impl::UploadToInteropImage(
    const uint8_t *_rgba, uint32_t _w, uint32_t _h)
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  if (!this->interopImage || !_rgba || _w == 0u || _h == 0u ||
      this->interopWidth != _w || this->interopHeight != _h)
    return;
  AZ::RHI::Image *uploadImage = this->interopImage->GetRHIImage();
  AZ::RHI::DeviceImageSubresourceLayout devLayout =
      AZ::RHI::GetImageSubresourceLayout(AZ::RHI::Size(_w, _h, 1u),
          AZ::RHI::Format::R8G8B8A8_UNORM);
  AZ::RHI::ImageSubresourceLayout srcLayout;
  srcLayout.Init(uploadImage->GetDeviceMask(), devLayout);
  AZ::RHI::ImageUpdateRequest update;
  update.m_image = uploadImage;
  update.m_sourceData = _rgba;
  update.m_sourceSubresourceLayout = srcLayout;
  this->interopImage->UpdateImageContents(update);
#else
  (void)_rgba; (void)_w; (void)_h;
#endif
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::EnsureInteropSemaphore()
{
#if !defined(GZ_O3DE_INTEROP_BUILD)
  return false;
#else
  if (this->renderFinishedFence && this->interopSemaphoreFd >= 0)
    return true;

  if (!this->renderFinishedFence)
  {
    // A timeline-semaphore fence. The external-handle bus (connected pre-device-
    // creation) made Atom's timeline semaphores exportable, so its native
    // VkSemaphore can be exported as an OPAQUE_FD below. usedForWaitingOnDevice
    // = true is REQUIRED: it selects Atom's TimelineSemaphoreFence impl (the
    // default false gives a BinaryFence, whose native handle is a VkFence, not a
    // VkSemaphore -- GetFenceNativeHandle asserts on it). The consumer (Qt's GPU)
    // waits on the exported timeline semaphore, so "waited for on the device" fits.
    this->renderFinishedFence = aznew AZ::RHI::Fence;
    const AZ::RHI::ResultCode rc = this->renderFinishedFence->Init(
        AZ::RHI::MultiDevice::AllDevices, AZ::RHI::FenceState::Reset,
        /*usedForWaitingOnDevice*/ true);
    if (rc != AZ::RHI::ResultCode::Success)
    {
      std::fprintf(stderr,
          "[gz-o3de] interop: render-finished Fence::Init failed (%d)\n",
          static_cast<int>(rc));
      this->renderFinishedFence = nullptr;
      return false;
    }
  }

  const int deviceIndex = AZ::RHI::MultiDevice::DefaultDeviceIndex;
  AZ::RHI::Ptr<AZ::RHI::DeviceFence> devFence =
      this->renderFinishedFence->GetDeviceFence(deviceIndex);
  AZ::RHI::Device *device =
      AZ::RHI::RHISystemInterface::Get()->GetDevice(deviceIndex);
  if (!devFence || !device)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: no device fence/device for the semaphore\n");
    return false;
  }
  const VkDevice vkDevice = AZ::Vulkan::GetDeviceNativeHandle(*device);
  const VkSemaphore sem = AZ::Vulkan::GetFenceNativeHandle(*devFence);
  if (vkDevice == VK_NULL_HANDLE || sem == VK_NULL_HANDLE)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: null VkDevice/VkSemaphore from fence accessors "
        "(a timeline-semaphore fence is required)\n");
    return false;
  }

  // vkGetSemaphoreFdKHR is a device extension entry point; resolve it through the
  // already-loaded Vulkan loader (mirrors the vkGetMemoryFdKHR resolution).
  void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
  auto getDeviceProcAddr = loader
      ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            dlsym(loader, "vkGetDeviceProcAddr"))
      : nullptr;
  auto getSemaphoreFd = getDeviceProcAddr
      ? reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            getDeviceProcAddr(vkDevice, "vkGetSemaphoreFdKHR"))
      : nullptr;
  if (!getSemaphoreFd)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: could not resolve vkGetSemaphoreFdKHR\n");
    return false;
  }
  VkSemaphoreGetFdInfoKHR getFdInfo{};
  getFdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
  getFdInfo.semaphore = sem;
  getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  const VkResult res = getSemaphoreFd(vkDevice, &getFdInfo, &fd);
  if (res != VK_SUCCESS || fd < 0)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: vkGetSemaphoreFdKHR FAILED (VkResult=%d, fd=%d) -- "
        "the timeline semaphore is not exportable; check the bus handler\n",
        static_cast<int>(res), fd);
    return false;
  }
  this->interopSemaphoreFd = fd;
  std::fprintf(stderr,
      "[gz-o3de] interop: render-finished timeline semaphore exported "
      "(VkSemaphore=%p, fd=%d)\n", reinterpret_cast<void *>(sem), fd);
  return true;
#endif
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::EnsureFenceSignalScope()
{
#if !defined(GZ_O3DE_INTEROP_BUILD)
  return false;
#else
  if (this->fenceSignalScope)
    return true;
  if (!this->renderFinishedFence)
    return false;
  // A no-data scope: its only work is to copy-read the interop image and signal
  // the fence (see FenceSignalPrepare). Compile/Execute are empty -- we record no
  // GPU commands; the copy-read attachment exists purely to anchor the scope in
  // the frame graph (so it is scheduled + submitted, not culled) and to force the
  // read-after-write ordering against the pipeline's render into the same image.
  this->fenceSignalScope =
      AZStd::make_shared<AZ::RHI::ScopeProducerFunctionNoData>(
          AZ::RHI::ScopeId("GzO3deFenceSignal"),
          AZStd::bind(&O3deBackend::Impl::FenceSignalPrepare, this,
              AZStd::placeholders::_1),
          [](const AZ::RHI::FrameGraphCompileContext &) {},
          [](const AZ::RHI::FrameGraphExecuteContext &) {});
  return this->fenceSignalScope != nullptr;
#endif
}

//////////////////////////////////////////////////
void O3deBackend::Impl::FenceSignalPrepare(
    [[maybe_unused]] AZ::RHI::FrameGraphInterface _frameGraph)
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  if (!this->interopImage || !this->renderFinishedFence)
    return;
  // Copy-read the interop image: a read of the attachment the pipeline writes
  // this frame, so the scheduler places this scope after the render (RAW), and
  // the fence -- signalled at the end of this scope -- is therefore signalled
  // only once the scene has been rendered into the shared image.
  AZ::RHI::ImageScopeAttachmentDescriptor desc{
      this->interopImage->GetAttachmentId() };
  _frameGraph.UseCopyAttachment(desc, AZ::RHI::ScopeAttachmentAccess::Read);
  _frameGraph.SignalFence(*this->renderFinishedFence);
#endif
}

//////////////////////////////////////////////////
void O3deBackend::Impl::OnFramePrepare(
    [[maybe_unused]] AZ::RHI::FrameGraphBuilder &_builder)
{
#if defined(GZ_O3DE_INTEROP_BUILD)
  // Fires every RHI frame after RPI passes register. Import the fence-signal
  // scope only on the flagged tick (one per rendered frame) so the timeline value
  // advances exactly once per frame; the interop image must already exist.
  if (this->signalFenceThisTick && this->fenceSignalScope &&
      this->interopImageReady)
  {
    _builder.ImportScopeProducer(*this->fenceSignalScope);
  }
#endif
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::RecreateInteropPipeline(uint32_t _w, uint32_t _h)
{
#if !defined(GZ_O3DE_INTEROP_BUILD)
  (void)_w; (void)_h;
  return false;
#else
  // (Re)create the exportable image (+ its asset) at the requested size; reuses
  // the existing one when the size is unchanged. Bumps the generation + exports a
  // fresh FD on a real (re)create, which the consumer notices and re-imports.
  const bool sameSize =
      this->interopImage && this->interopImageReady &&
      this->interopWidth == _w && this->interopHeight == _h;
  if (!this->EnsureInteropImage(_w, _h) || !this->interopImageAsset.IsReady())
    return false;
  if (sameSize && this->pipeline)
    return true;  // image + pipeline already at this size

  auto *rpiSystem = AZ::RPI::RPISystemInterface::Get();

  // Swap out the previous interop pipeline (the image -- and thus the pipeline
  // output it was built around -- changed size).
  if (this->pipeline)
  {
    this->scene->RemoveRenderPipeline(this->pipeline->GetId());
    this->pipeline = nullptr;
  }

  // MainPipeline as the root template renders the full scene (incl. AuxGeom)
  // straight into its PipelineOutput slot, which CreateRenderPipelineForImage
  // binds to our exportable image. MainPipeline is Atom's deferred pipeline: its
  // G-buffer/lighting shaders REQUIRE multisampled inputs (they Load() the depth/
  // albedo/normal/specular targets per sample), so the pipeline must run at >1x
  // MSAA. At 1x those image bindings fail SRG validation, the draws sample unbound
  // descriptors, and the GPU faults -- losing the *shared physical device* (seen
  // as a Qt-side "Device loss detected in vkQueueSubmit"). 4x matches the proven
  // CPU-readback path (MainPipelineRenderToTexture wraps this same MainPipeline):
  // MainPipeline resolves its multisampled targets down to the single-sample
  // PipelineOutput, so the single-sample image we bind there is the right resolve
  // target -- no extra resolve step to manage.
  AZ::RPI::RenderPipelineDescriptor pipelineDesc;
  pipelineDesc.m_mainViewTagName = "MainCamera";
  pipelineDesc.m_name = AZStd::string::format(
      "GzO3deInteropPipeline_%llu",
      static_cast<unsigned long long>(this->interopGeneration));
  pipelineDesc.m_rootPassTemplate = "MainPipeline";
  pipelineDesc.m_renderSettings.m_multisampleState = AZ::RHI::MultisampleState(
      /*samples*/ static_cast<uint16_t>(4), /*quality*/ static_cast<uint16_t>(0));

  this->pipeline = AZ::RPI::RenderPipeline::CreateRenderPipelineForImage(
      pipelineDesc, this->interopImageAsset);
  if (!this->pipeline)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: CreateRenderPipelineForImage failed (%ux%u)\n",
        _w, _h);
    return false;
  }
  this->scene->AddRenderPipeline(this->pipeline);
  if (this->view)
    this->pipeline->SetDefaultView(this->view);

  // Select the MSAA shader supervariant and mark the new pipeline's passes for
  // the multisampled rebuild. Must run after AddRenderPipeline (it walks the
  // registered scenes' pipelines); the caller's settle ticks flush the rebuild.
  // Re-applied on every (re)create so a resize-driven pipeline is multisampled
  // too. Mirrors the CPU-readback SetupScene step.
  rpiSystem->SetApplicationMultisampleState(
      this->pipeline->GetRenderSettings().m_multisampleState);

  std::fprintf(stderr,
      "[gz-o3de] interop: Stage B pipeline rendering into exportable image "
      "(%ux%u, gen=%llu, 4x MSAA)\n", _w, _h,
      static_cast<unsigned long long>(this->interopGeneration));
  return true;
#endif
}

//////////////////////////////////////////////////
void O3deBackend::Impl::ProveFdExportOnce()
{
  if (this->interopFdProven)
    return;
  this->interopFdProven = true;  // attempt once regardless of outcome

#if !defined(GZ_O3DE_INTEROP_BUILD)
  // GZ_O3DE_INTEROP was set at runtime, but this plugin was built without
  // -DGZ_O3DE_INTEROP=ON, so the native-handle accessors (and the gem patch they
  // need) are not compiled in. The bus handlers above still ran (images are
  // exportable), but we cannot fetch the FD here.
  std::fprintf(stderr,
      "[gz-o3de] interop: GZ_O3DE_INTEROP set but plugin built without "
      "-DGZ_O3DE_INTEROP=ON; FD-export probe skipped\n");
  return;
#else
  // Create the initial exportable image at probe size and fill it with a
  // deterministic gradient (R=x, G=y, B=128). This proves FD export works and
  // gives the GL/Vulkan self-tests a known pattern to round-trip before any live
  // frame arrives; the first live frame recreates the image at camera size.
  const uint32_t kW = 256u;
  const uint32_t kH = 256u;
  if (!this->EnsureInteropImage(kW, kH))
  {
    std::fprintf(stderr, "[gz-o3de] interop: probe image creation failed\n");
    return;
  }
  std::vector<uint8_t> pattern(static_cast<size_t>(kW) * kH * 4u);
  for (uint32_t y = 0u; y < kH; ++y)
  {
    for (uint32_t x = 0u; x < kW; ++x)
    {
      uint8_t *px = &pattern[(static_cast<size_t>(y) * kW + x) * 4u];
      px[0] = static_cast<uint8_t>(x);
      px[1] = static_cast<uint8_t>(y);
      px[2] = 128u;
      px[3] = 255u;
    }
  }
  this->UploadToInteropImage(pattern.data(), kW, kH);
  std::fprintf(stderr,
      "[gz-o3de] interop: probe gradient upload requested (%ux%u)\n", kW, kH);
  // Tick so the async upload copy runs before the FD is handed to a consumer.
  for (int i = 0; i < 5; ++i)
  {
    this->app->PumpSystemEventLoopUntilEmpty();
    this->app->TickSystem();
    this->app->Tick();
  }
#endif  // GZ_O3DE_INTEROP_BUILD
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
bool O3deBackend::GetInteropImport(O3deInteropImport &_out)
{
  Impl &d = *this->dataPtr;
  std::lock_guard<std::mutex> lock(d.mutex);
  if (!d.interopImageReady || d.interopFd < 0)
    return false;

  // Hand the caller its own dup of the export FD (they own and close it). The
  // backend keeps d.interopFd as the canonical handle. dup() is just an OS fd
  // operation -- no O3DE/Vulkan call -- so this is safe off the render thread.
  const int dupFd = ::dup(d.interopFd);
  if (dupFd < 0)
    return false;
  _out.fd = dupFd;
  _out.width = d.interopWidth;
  _out.height = d.interopHeight;
  _out.allocationSize = d.interopAllocSize;
  _out.allocationOffset = d.interopAllocOffset;
  _out.generation = d.interopGeneration;
  _out.live = d.interopLive;

  // Stage B render-finished sync: hand the caller a dup of the exported timeline
  // semaphore FD (it owns and closes it) plus the value the GPU will signal for
  // the latest frame. The consumer imports the FD once (the handle is stable) and
  // waits on the per-frame value before sampling. interopSemaphoreReady gates the
  // advertisement (env GZ_O3DE_INTEROP_SEM): until the producer signal path is
  // verified (Step A), or on the static-probe path, we advertise -1 so the
  // consumer falls back to no cross-device wait (cannot hang on the semaphore).
  _out.semaphoreFd =
      (d.interopSemaphoreReady && d.interopSemaphoreFd >= 0)
          ? ::dup(d.interopSemaphoreFd)
          : -1;
  _out.semaphoreWaitValue = d.interopSemaphoreValue;
  return true;
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

  // Register an exit handler exactly once, after a successful bootstrap.
  //
  // Process exit with a live O3DE/Vulkan runtime crashes, and there is no clean
  // path around it from here:
  //
  //   * Tearing the runtime down (app->Stop()) crashes inside O3DE's own
  //     RHISystem/Vulkan teardown: the release-queue flush lazily re-inits the
  //     AsyncUploadQueue against a null staging-buffer pool after the device is
  //     already reported "Device lost" (SIGSEGV in DeviceBufferPool, deep in
  //     RPISystem::Shutdown). This is the upstream bug the original design
  //     avoided by never tearing down.
  //   * Leaving the runtime leaked (our design) instead crashes in the NVIDIA
  //     driver's *own* atexit handler (libnvidia-glcore / libGLX_nvidia) when it
  //     tears down a still-live Vulkan device during C++ static destruction.
  //
  // Neither crash is in our code; both are GPU driver/engine teardown bugs that
  // fire only at process exit, after all rendering is done. The robust, common
  // mitigation for GPU apps is to skip the crashing destructors entirely with a
  // fast process exit. So: join our render thread (removing the only race we
  // own -- a foreign thread ticking the runtime during teardown -- which was
  // the original SIGABRT), then std::quick_exit() to terminate *before* the
  // buggy O3DE/NVIDIA static/atexit destructors run.
  //
  // Trade-off (acceptable for this experimental PoC): quick_exit() skips the
  // remaining atexit handlers and static destructors of the *host* process
  // (gz-gui / gz-sim), so e.g. Qt teardown does not run. This handler is
  // registered LIFO and only after a successful bootstrap, so it runs before
  // O3DE's destructors and never fires if the runtime never came up (letting
  // the host recover / fall back to another engine on a failed load).
  if (d.bootstrapOk)
  {
    static std::once_flag atexitFlag;
    std::call_once(atexitFlag, []()
    {
      std::atexit([]()
      {
        // Stop + join the render thread (clean, on its owning thread), then
        // bail out of the process before any GPU teardown destructor can crash.
        O3deBackend::Instance().Shutdown();
        std::fprintf(stderr,
            "[gz-o3de] quick_exit: skipping O3DE/GPU teardown destructors\n");
        std::quick_exit(0);
      });
    });
  }
  return d.bootstrapOk;
}

//////////////////////////////////////////////////
void O3deBackend::Shutdown()
{
  Impl &d = *this->dataPtr;

  // Signal the render thread to leave its loop and tear the runtime down. The
  // thread does all the O3DE work (it created the runtime); this thread only
  // flips the flag and waits. Idempotent: after the join the thread is no
  // longer joinable, so a second call is a no-op.
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    if (!d.renderThread.joinable())
      return;
    d.ready = false;  // reject any further RenderFrame() calls
    d.stop.store(true);
  }
  d.inputCv.notify_all();
  d.renderThread.join();
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
    else if (!(this->interopLive && std::getenv("GZ_O3DE_NO_IDLE_TICK")))
    {
      // Idle: keep the engine ticking so async loads/streaming progress.
      this->app->PumpSystemEventLoopUntilEmpty();
      this->app->TickSystem();
      this->app->Tick();
    }
    // DIAGNOSTIC (interop bring-up): with GZ_O3DE_NO_IDLE_TICK set on the live
    // path we do NOT tick the app while idle. app->Tick() runs Atom's RHI frame
    // scheduler, which submits GPU work on Atom's device every ~16ms -- including
    // touching the live pipeline's imported output attachment -- while the
    // consumer (Qt, a separate VkDevice) is concurrently sampling that same shared
    // image. This A/B-tests whether that unsynchronized concurrent cross-device
    // access is what hangs the GPU (~5s TDR) rather than a missing consumer-side
    // semaphore. The 16ms inputCv wait above still throttles the loop.
  }

  // Stop was signalled (process exit). Tear the runtime down here, on the
  // thread that created it, before O3DE's static destructors run.
  this->TeardownOnThread();
}

//////////////////////////////////////////////////
void O3deBackend::Impl::TeardownOnThread()
{
  if (!this->app)
    return;

  // Stop driving the offscreen pipeline so no GPU work is queued from here on.
  if (this->pipeline)
    this->pipeline->RemoveFromRenderTick();

  // The GameApplication / RPISystem is intentionally NOT stopped or destroyed:
  // O3DE's Vulkan RHI teardown of a live RPISystem crashes (release-queue flush
  // lazily re-inits the AsyncUploadQueue against a null staging pool after the
  // device is already lost). The original race that the SIGABRT came from was a
  // *foreign* thread (this one) ticking the runtime while the process tore down;
  // joining this thread before static teardown removes that race. The runtime
  // is leaked deliberately and the OS reclaims it at exit.
  std::fprintf(stderr, "[gz-o3de] render thread stopped; runtime left alive\n");
}

//////////////////////////////////////////////////
bool O3deBackend::Impl::BootstrapOnThread()
{
  using FixedValueString = AZ::SettingsRegistryInterface::FixedValueString;

  // SSAA factor: GZ_O3DE_SSAA overrides the default (1 disables anti-aliasing,
  // up to kMaxSsaaScale). Read once here so the whole render loop is consistent.
  if (const char *ssaaEnv = std::getenv("GZ_O3DE_SSAA"))
  {
    const long v = std::strtol(ssaaEnv, nullptr, 10);
    if (v >= 1 && v <= static_cast<long>(kMaxSsaaScale))
      this->ssaaScale = static_cast<uint32_t>(v);
  }
  std::fprintf(stderr, "[gz-o3de] SSAA scale = %u\n", this->ssaaScale);

  // M4 interop: opt-in request that Atom create exportable images + semaphores.
  this->interop = (std::getenv("GZ_O3DE_INTEROP") != nullptr);
  // Phase 2a live-scene-into-shared-image path (needs #26 sync; see member doc).
  this->interopLive =
      this->interop && (std::getenv("GZ_O3DE_INTEROP_LIVE") != nullptr);
  // Stage B copies the resolved single-sample "Output" attachment straight into
  // the exportable image with a plain (non-scaling) Vulkan copy, so the source
  // and destination must be the same size. SSAA renders "Output" at scale x the
  // logical size; force 1x on the live path so the copy stays 1:1 (4x MSAA still
  // applies inside the pipeline, so anti-aliasing is preserved).
  if (this->interopLive && this->ssaaScale != 1u)
  {
    std::fprintf(stderr,
        "[gz-o3de] interop: forcing SSAA 1x for the live render-into-image path "
        "(was %u)\n", this->ssaaScale);
    this->ssaaScale = 1u;
  }

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

  // Connect the external-handle bus BEFORE Start(): the RHI device (and its VMA
  // allocator, which captures the external memory handle types once at creation)
  // is built during Start(). The bus context lives in the AZ environment, which
  // the GameApplication ctor above already brought up, so the handler is visible
  // to the Vulkan RHI gem when it broadcasts during device creation.
  if (this->interop)
  {
    // The class handles two buses; BusConnect() is ambiguous, so qualify each.
    this->externalHandleProvider
        .AZ::Vulkan::ExternalHandleRequirementBus::Handler::BusConnect();
    this->externalHandleProvider
        .AZ::Vulkan::DeviceRequirementBus::Handler::BusConnect();
    std::fprintf(stderr,
        "[gz-o3de] interop: ExternalHandleRequirementBus + DeviceRequirementBus "
        "handlers connected (GZ_O3DE_INTEROP)\n");
  }

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
// Demo aid: gz-gui's MinimalScene starts with an empty scene (primitives
// normally come from gz-sim); when GZ_O3DE_DEMO_SHAPES is set and there are no
// primitives, inject a box/sphere/cylinder trio so the live viewer has
// something to show. Shared by the readback (RenderFrame) and native-interop
// (RenderFrameForInterop) paths.
static void MaybeInjectDemoShapes(std::vector<O3deShapeData> &_shapes)
{
  if (!_shapes.empty() || !std::getenv("GZ_O3DE_DEMO_SHAPES"))
    return;
  O3deShapeData box;
  box.type = O3deShapeData::Type::BOX;
  box.pos[0] = 0.0; box.pos[1] = 1.5; box.pos[2] = 0.5;
  box.color[0] = 1.0f; box.color[1] = 0.0f; box.color[2] = 0.0f;
  _shapes.push_back(box);

  O3deShapeData sphere;
  sphere.type = O3deShapeData::Type::SPHERE;
  sphere.pos[0] = 0.0; sphere.pos[1] = 0.0; sphere.pos[2] = 0.5;
  sphere.color[0] = 0.0f; sphere.color[1] = 1.0f; sphere.color[2] = 0.0f;
  _shapes.push_back(sphere);

  O3deShapeData cylinder;
  cylinder.type = O3deShapeData::Type::CYLINDER;
  cylinder.pos[0] = 0.0; cylinder.pos[1] = -1.5; cylinder.pos[2] = 0.5;
  cylinder.scale[2] = 1.5;
  cylinder.color[0] = 0.0f; cylinder.color[1] = 0.0f; cylinder.color[2] = 1.0f;
  _shapes.push_back(cylinder);
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
  // the render thread).
  std::vector<O3deShapeData> shapes = _shapes;
  MaybeInjectDemoShapes(shapes);

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
bool O3deBackend::RenderFrameForInterop(const O3deCameraData &_camera,
    const std::vector<O3deShapeData> &_shapes,
    uint32_t _width, uint32_t _height)
{
  Impl &d = *this->dataPtr;
  // Only meaningful when the live path is enabled; the render thread then
  // publishes each frame into the exportable image (see RenderOneFrame). No CPU
  // readout. With only GZ_O3DE_INTEROP (no _LIVE) the consumer samples the
  // stable static probe and this is a no-op (avoids the #26 device-loss race).
  if (!d.ready || !d.interopLive || _width == 0u || _height == 0u)
    return false;

  std::vector<O3deShapeData> shapes = _shapes;
  MaybeInjectDemoShapes(shapes);

  // Post the input to the render thread and wait for a frame rendered after it
  // (the frame is published into the shared image by RenderOneFrame()).
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
      [&d, startSeq]() { return d.frameSeq > startSeq; });
  if (!got)
    std::fprintf(stderr,
        "[gz-o3de] interop: RenderFrameForInterop TIMED OUT after 5s waiting "
        "for frame > %llu (frameSeq=%llu)\n",
        static_cast<unsigned long long>(startSeq),
        static_cast<unsigned long long>(d.frameSeq));
  return got;
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

#if defined(GZ_O3DE_INTEROP_BUILD)
  if (this->interopLive)
  {
    // Stage B live render: (re)size the exportable image + its pipeline to the
    // requested logical resolution (SSAA is forced 1x on this path), render the
    // scene straight into the image, then host-sync so the consumer samples a
    // complete frame. No SSAA downsample, no CPU readback.
    uint32_t lw =
        (this->reqWidth > 0u) ? this->reqWidth : this->interopWidth;
    uint32_t lh =
        (this->reqHeight > 0u) ? this->reqHeight : this->interopHeight;
    // ISOLATION (GZ_O3DE_NO_RESIZE): keep rendering at the already-created image
    // size, so the exportable image is never recreated (gen stays 1) and the
    // consumer imports it exactly once. Distinguishes a resize / re-import bug
    // (works with this set) from a live-render / compression bug (still fails).
    if (this->interopImageReady && this->interopWidth > 0u &&
        std::getenv("GZ_O3DE_NO_RESIZE"))
    {
      lw = this->interopWidth;
      lh = this->interopHeight;
    }
    if (lw == 0u || lh == 0u)
      return false;
    if (lw != this->interopWidth || lh != this->interopHeight || !this->pipeline)
    {
      if (!this->RecreateInteropPipeline(lw, lh))
        return false;
      this->outputWidth = lw;
      this->outputHeight = lh;
      // Settle the freshly (re)created pipeline's pass + MSAA shader-variant builds
      // before rendering the frame the consumer will sample (they self-pump on this
      // thread). Deferred here from SetupScene so the exportable image is created
      // ONCE, at the real camera size -- the first frame creates it instead of an
      // initial placeholder->live resize that would recreate it under the consumer.
      this->ApplyCamera(lw, lh);
      this->pipeline->AddToRenderTick();
      for (int i = 0; i < 5; ++i)
      {
        this->SubmitPrimitives();
        this->app->PumpSystemEventLoopUntilEmpty();
        this->app->TickSystem();
        this->app->Tick();
      }
      this->pipeline->RemoveFromRenderTick();
    }
    this->ApplyCamera(lw, lh);

    // Render the scene into the exportable image. AuxGeom is re-submitted each
    // tick (the draw queue is consumed per rendered frame).
    this->pipeline->AddToRenderTick();
    for (int k = 0; k < 3; ++k)
    {
      this->SubmitPrimitives();
      // #26: import the fence-signal scope on exactly the last render tick, so
      // its SignalFence runs once per frame after the scene is drawn into the
      // shared image. OnFramePrepare (fired during app->Tick) checks this flag.
      this->signalFenceThisTick =
          (k == 2) && this->fenceSignalScope && this->renderFinishedFence;
      this->app->PumpSystemEventLoopUntilEmpty();
      this->app->TickSystem();
      this->app->Tick();
    }
    this->signalFenceThisTick = false;
    this->pipeline->RemoveFromRenderTick();

    // Host-sync (interim, until the render-finished semaphore #26 is wired via a
    // custom RPI::Pass): block until the GPU has finished rendering into the
    // exportable image before the consumer -- which unblocks when frameSeq bumps
    // -- samples it. Removes the cross-device write/read race that lost the
    // device; the consumer's EXTERNAL ownership acquire then settles the layout.
    VkResult waitResult = VK_SUCCESS;
    if (AZ::RHI::Device *device = AZ::RHI::RHISystemInterface::Get()->GetDevice(
            AZ::RHI::MultiDevice::DefaultDeviceIndex))
    {
      const VkDevice vkDevice = AZ::Vulkan::GetDeviceNativeHandle(*device);
      if (vkDevice != VK_NULL_HANDLE)
        waitResult = vkDeviceWaitIdle(vkDevice);
    }

    // #26: after host-sync the frame is GPU-complete, so read the value the
    // render-finished fence reached this frame and confirm it actually signalled.
    // signaledValue is advertised to the consumer (semaphoreWaitValue); fenceState
    // == Signaled is the Step-A proof that the GPU signal path works. Reset()
    // below arms the next monotonic value -- but only when we actually signalled,
    // so a stalled signal never advances the value past what the GPU reaches.
    uint64_t signaledValue = this->interopSemaphoreValue;
    bool fenceSignalled = false;
    if (this->renderFinishedFence)
    {
      AZ::RHI::Ptr<AZ::RHI::DeviceFence> devFence =
          this->renderFinishedFence->GetDeviceFence(
              AZ::RHI::MultiDevice::DefaultDeviceIndex);
      if (devFence)
      {
        signaledValue = AZ::Vulkan::GetFencePendingValue(*devFence);
        fenceSignalled =
            (devFence->GetFenceState() == AZ::RHI::FenceState::Signaled);
      }
    }

    // Periodic liveness log (first few frames + every 20th, see logThis) + always
    // on a host-sync failure: frame rate, generation, the #26 fence value consumer
    // waits on, and vkDeviceWaitIdle's result (a VK_ERROR_DEVICE_LOST here would
    // flag a producer-side fault).
    if (logThis || waitResult != VK_SUCCESS || !fenceSignalled)
    {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - frameStart).count();
      std::fprintf(stderr,
          "[gz-o3de] interop live frame %d: %ux%u gen=%llu %lld ms waitIdle=%d "
          "fence=%s value=%llu\n",
          frameNum, lw, lh,
          static_cast<unsigned long long>(this->interopGeneration),
          static_cast<long long>(ms), static_cast<int>(waitResult),
          fenceSignalled ? "Signaled" : "Reset",
          static_cast<unsigned long long>(signaledValue));
    }

    {
      std::lock_guard<std::mutex> lock(this->mutex);
      this->latestWidth = lw;
      this->latestHeight = lh;
      // Only publish a value the GPU actually reached, so a consumer waiting on
      // it (Step B) cannot block on an unreachable timeline value.
      if (fenceSignalled)
        this->interopSemaphoreValue = signaledValue;
      ++this->frameSeq;
    }
    this->outputCv.notify_all();

    // Advance the timeline fence to the next (strictly greater) value for the
    // next frame. Reset() is host-side bookkeeping (m_pendingValue++) -- safe
    // after the host-sync above -- and only valid once this frame's value was
    // reached, hence gated on fenceSignalled.
    if (fenceSignalled && this->renderFinishedFence)
      this->renderFinishedFence->Reset();
    return true;
  }
#endif

  // Size the offscreen target to the requested resolution whenever it changes
  // (e.g. gz-gui's window is resized). A resize recreates the pass attachments,
  // so render a few warm-up ticks at the new size before requesting a capture
  // (capturing an attachment that has not yet been rendered at the new size
  // never completes). Resizing is safe here because all O3DE work runs on this
  // one thread: the synchronous shader loads a pass rebuild can trigger
  // self-pump and complete (from a foreign thread they would deadlock).
  // SSAA: outputWidth/outputHeight track the *supersampled* render-target size
  // (ssaaScale x the requested logical resolution). The capture is downsampled
  // back to the logical size when the frame is published.
  const uint32_t scale = this->ssaaScale;
  const uint32_t logicalW = (this->reqWidth > 0u)
      ? this->reqWidth : (this->outputWidth / scale);
  const uint32_t logicalH = (this->reqHeight > 0u)
      ? this->reqHeight : (this->outputHeight / scale);
  const uint32_t w = logicalW * scale;
  const uint32_t h = logicalH * scale;
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

  // SSAA resolve: box-downsample the supersampled capture (captureWidth x
  // captureHeight) to the logical resolution -- each output pixel is the average
  // of a scale x scale block. This is what applies anti-aliasing to the whole
  // image, AuxGeom included. captureWidth/Height are exact multiples of scale
  // (we sized the target that way), so no source pixels are lost. With scale==1
  // this is a straight copy (no anti-aliasing).
  const uint32_t outW = this->captureWidth / scale;
  const uint32_t outH = this->captureHeight / scale;
  std::vector<uint8_t> resolved(static_cast<size_t>(outW) * outH * 4u);
  const size_t srcStride = static_cast<size_t>(this->captureWidth) * 4u;
  const uint32_t block = scale * scale;
  for (uint32_t oy = 0u; oy < outH; ++oy)
  {
    for (uint32_t ox = 0u; ox < outW; ++ox)
    {
      uint32_t acc[4] = {0u, 0u, 0u, 0u};
      for (uint32_t sy = 0u; sy < scale; ++sy)
      {
        const uint8_t *src = this->captureBuffer.data() +
            (static_cast<size_t>(oy) * scale + sy) * srcStride +
            static_cast<size_t>(ox) * scale * 4u;
        for (uint32_t sx = 0u; sx < scale; ++sx)
        {
          acc[0] += src[sx * 4u + 0u];
          acc[1] += src[sx * 4u + 1u];
          acc[2] += src[sx * 4u + 2u];
          acc[3] += src[sx * 4u + 3u];
        }
      }
      uint8_t *dst = resolved.data() +
          (static_cast<size_t>(oy) * outW + ox) * 4u;
      dst[0] = static_cast<uint8_t>(acc[0] / block);
      dst[1] = static_cast<uint8_t>(acc[1] / block);
      dst[2] = static_cast<uint8_t>(acc[2] / block);
      dst[3] = static_cast<uint8_t>(acc[3] / block);
    }
  }

  // Note: the GZ_O3DE_INTEROP_LIVE path returns earlier (Stage B renders the
  // scene directly into the exportable image, zero-copy); this tail is the
  // CPU-readback path for RenderFrame()/the static probe only.

  // Publish the completed frame for RenderFrame() to pick up.
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->latestFrame.swap(resolved);
    this->latestWidth = outW;
    this->latestHeight = outH;
    ++this->frameSeq;
  }
  this->outputCv.notify_all();
  return true;
}

}  // namespace rendering
}  // namespace gz
