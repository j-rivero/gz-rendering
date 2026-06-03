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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/Pass/Specific/RenderToTexturePass.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomFeatureProcessorInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomDraw.h>
// M6 lights (Atom Common, header-only interfaces): the 3 light feature
// processors we acquire/release/sync handles against. PhotometricValue
// supplies the strongly-typed colour types Set*Intensity takes.
#include <Atom/Feature/CoreLights/DirectionalLightFeatureProcessorInterface.h>
#include <Atom/Feature/CoreLights/SimplePointLightFeatureProcessorInterface.h>
#include <Atom/Feature/CoreLights/SimpleSpotLightFeatureProcessorInterface.h>
#include <Atom/Feature/Shadows/ProjectedShadowFeatureProcessorInterface.h>
#include <Atom/Feature/CoreLights/ShadowConstants.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/RPI.Reflect/Asset/AssetUtils.h>
#include <Atom/RPI.Reflect/Model/ModelAsset.h>
// M9: procedural model construction from raw vertex/index buffers, used to turn
// a gz::common::Mesh (from a gz-rendering MeshDescriptor) into an Atom model at
// runtime without going through the offline Asset Processor / .azmodel cooking.
// CreateBufferAsset (from ModelAssetHelpers) wraps the raw buffers; the LOD/model
// creators assemble + finalise the asset (their End() marks it Ready, which
// MeshFeatureProcessor::AcquireMesh requires -- ModelAssetHelpers::CreateModel
// alone leaves the model non-Ready).
#include <Atom/RPI.Reflect/Model/ModelAssetHelpers.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/ResourcePoolAssetCreator.h>
#include <Atom/RHI.Reflect/BufferViewDescriptor.h>
#include <Atom/RHI.Reflect/ShaderSemantic.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/Feature/CoreLights/PhotometricValue.h>
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

// M9: gz-common mesh geometry source. SubMesh exposes per-vertex
// position/normal/UV and the index list that the Atom ModelAssetHelpers builder
// consumes; MeshManager supplies a built-in primitive for the demo gate.
#include <gz/common/Mesh.hh>
#include <gz/common/SubMesh.hh>
#include <gz/common/MeshManager.hh>

#include "O3deBackend.hh"
#include "O3deGlInterop.hh"  // plain-types declaration; no GL headers leak here
#include "O3deVkInterop.hh"  // plain-types declaration; no Vulkan headers leak here
#include "renderdoc_app.h"   // in-app RenderDoc capture trigger (diagnostic only)

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

/// \brief Plain CPU-side geometry for one mesh id (M9), extracted from a
/// gz::common::Mesh on the gz thread by RegisterMesh and consumed on the render
/// thread to build the Atom model. Holds the merged, Atom-ready interleaved
/// streams (one element per vertex: positions x3, normals x3, tangents x4,
/// bitangents x3, uvs x2) plus the index list, already re-wound for Atom's
/// front-face convention. No Atom/AzCore types -- safe to build off-thread.
struct MeshGeometryCpu
{
  std::vector<uint32_t> indices;
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> tangents;
  std::vector<float> bitangents;
  std::vector<float> uvs;

  bool Empty() const { return this->positions.empty() || this->indices.empty(); }
};

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
  public: std::vector<O3deLightData> pendingLights;
  public: std::vector<O3deMeshData> pendingMeshes;  // M9
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
  public: std::vector<O3deLightData> lights;
  public: std::vector<O3deMeshData> meshes;  // M9
  public: uint32_t reqWidth = 0u;
  public: uint32_t reqHeight = 0u;

  // ---- M6 lights ----------------------------------------------------------
  // Cached feature-processor interface pointers (set in SetupScene after the
  // scene activates). The 3 light FPs are pre-registered with the scene at
  // line ~470 below; we just keep raw pointers for the per-frame Set*Data
  // calls. nullptr until SetupScene completes; SubmitLights() short-circuits
  // each branch independently if its FP isn't available.
  public: AZ::Render::DirectionalLightFeatureProcessorInterface *dirLightFp
      = nullptr;
  public: AZ::Render::SimplePointLightFeatureProcessorInterface *pointLightFp
      = nullptr;
  public: AZ::Render::SimpleSpotLightFeatureProcessorInterface *spotLightFp
      = nullptr;
  // M7 Phase A1: ProjectedShadowFP. Cached only -- Acquire/SetShadowProperties
  // wiring lands in Phase A2 once we have proof that registering this FP
  // doesn't grey-screen the demo the way DirectionalLightFP did.
  public: AZ::Render::ProjectedShadowFeatureProcessorInterface *
      projectedShadowFp = nullptr;
  // gz light id (stable across frames) -> Atom FP-issued handle. SubmitLights()
  // acquires on first sighting, syncs every frame, and releases when the id
  // disappears from the gathered set.
  public: std::unordered_map<uint32_t,
      AZ::Render::DirectionalLightFeatureProcessorInterface::LightHandle>
      dirLightHandles;
  public: std::unordered_map<uint32_t,
      AZ::Render::SimplePointLightFeatureProcessorInterface::LightHandle>
      pointLightHandles;
  public: std::unordered_map<uint32_t,
      AZ::Render::SimpleSpotLightFeatureProcessorInterface::LightHandle>
      spotLightHandles;
  // M7 Phase A2: paired projected-shadow handle per spot light. Same gz id
  // is the key in both maps so light + shadow are always acquired/released
  // together; the spot LightHandle drives lighting, the ShadowId drives
  // the depth-map projector.
  public: std::unordered_map<uint32_t,
      AZ::Render::ProjectedShadowFeatureProcessorInterface::ShadowId>
      spotShadowHandles;
  // M7 Phase B: MeshFP + demo mesh handles. The shadow pass writes a depth
  // map from each spot light's pose and the lighting pass samples it via
  // the matching light's transform -- but only Mesh draws (not AuxGeom)
  // participate in either pass. We acquire one static "caster" + one
  // "receiver" mesh at SetupScene time so the shadow projector has actual
  // geometry to interact with.
  public: AZ::Render::MeshFeatureProcessorInterface *meshFp = nullptr;
  public: AZStd::vector<
      AZ::Render::MeshFeatureProcessorInterface::MeshHandle> demoMeshHandles;

  // ---- M9 mesh instances (public CreateMesh path) -------------------------
  // CPU geometry registered out of band by RegisterMesh (gz thread), keyed by
  // gz id. Guarded by `mutex`: the gz thread writes here while the render
  // thread reads/erases it. The render thread builds the Atom model the first
  // time it sees the id (Atom asset construction must stay render-side).
  public: std::unordered_map<uint64_t, MeshGeometryCpu> meshGeometry;
  // Ids the gz thread asked to drop; the render thread releases their handles
  // in SubmitMeshes and clears this list. Guarded by `mutex`.
  public: std::vector<uint64_t> meshUnregister;
  // Render-thread-only caches: built model + live MeshFP handle per id.
  public: std::unordered_map<uint64_t,
      AZ::Data::Asset<AZ::RPI::ModelAsset>> meshModels;
  public: std::unordered_map<uint64_t,
      AZ::Render::MeshFeatureProcessorInterface::MeshHandle> meshHandles;
  // M10: per-mesh StandardPBR material instances, tinted from
  // O3deMeshData::color. meshMaterialAsset is the shared StandardPBR base
  // (basic_grey) loaded once; each mesh id gets its own Material::Create
  // instance so its baseColor.color can be set independently. Kept alive here
  // for as long as the handle exists; released alongside it.
  public: AZ::Data::Asset<AZ::RPI::MaterialAsset> meshMaterialAsset;
  public: std::unordered_map<uint64_t,
      AZ::Data::Instance<AZ::RPI::Material>> meshMaterials;
  /// \brief M9: acquire/update/release Atom mesh handles for this->meshes,
  /// mirroring SubmitLights()'s id-keyed lifecycle (render thread only).
  public: void SubmitMeshes();

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
  /// \brief Synchronise the current frame's lights to the 3 light feature
  /// processors: acquire a handle on first sighting, set its photometric
  /// colour + transform + attenuation + cone angles, and release handles
  /// whose id has disappeared from the gathered set. AuxGeom primitives do
  /// NOT consume these (debug-draw uses a fixed shader); the lights become
  /// visible once the scene has a shaded surface (M8 PBR / M9 meshes).
  public: void SubmitLights();
  // M7 Phase B: acquire the static demo caster + receiver meshes after the
  // FP pointers are cached. Idempotent: called from both SetupScene paths
  // but only the first call does work (the demoMeshHandles vector is the
  // gate). Skipped entirely when GZ_O3DE_DEMO_SHAPES is unset.
  public: void AcquireDemoMeshes();
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
  // re-enable rolled back: even with zero instances, registering its FP here
  // turns the existing live-display path entirely grey in the demo -- the
  // QSG offscreen FBO ends up sampling an unwritten gen of the imported
  // image. Root cause not yet narrowed; sticking with the original omission
  // and letting SubmitLights() short-circuit the DIRECTIONAL branch when its
  // FP pointer is null. The directional case will land alongside the M7
  // shadow work that needs the same FP.) plus AuxGeom for our shapes.
  AZStd::vector<AZStd::string> featureProcessors = {
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
    // M7 Phase A1: ProjectedShadowFP enables spot/disk shadow projectors.
    // No instances yet -- Phase A1 only verifies that registering the FP
    // does not regress the live-display path the way DirectionalLightFP did.
    "AZ::Render::ProjectedShadowFeatureProcessor",
  };

  // M8: directional "sun" light, env-gated for A/B testing. The M6-A rollback
  // omitted DirectionalLightFeatureProcessor because registering it "turned the
  // live-display path entirely grey" -- but the 2026-06-02 single-light-grey
  // investigation showed that grey is an INTERMITTENT QSG/present race,
  // independent of which lights or FPs are registered (it strikes 1-light and
  // 2-light launches alike). That makes the M6-A observation a likely
  // misattribution of the same intermittent grey. Re-enable the FP behind
  // GZ_O3DE_ENABLE_DIRECTIONAL so the grey rate can be measured WITH vs WITHOUT
  // it (centre-viewport crop) instead of trusting a single observation. Default
  // (env unset) is unchanged: FP omitted, SubmitLights short-circuits dir.
  if (std::getenv("GZ_O3DE_ENABLE_DIRECTIONAL"))
  {
    featureProcessors.push_back("AZ::Render::DirectionalLightFeatureProcessor");
    std::fprintf(stderr,
        "[gz-o3de] M8: DirectionalLightFeatureProcessor registered "
        "(GZ_O3DE_ENABLE_DIRECTIONAL set)\n");
  }

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

    // M6: cache the 3 light FP pointers. The scene's FP set is fixed at
    // create-time, so this is a one-shot lookup; SubmitLights() consults
    // them every frame to acquire/release/sync per-light handles.
    this->dirLightFp = this->scene->GetFeatureProcessor<
        AZ::Render::DirectionalLightFeatureProcessorInterface>();
    this->pointLightFp = this->scene->GetFeatureProcessor<
        AZ::Render::SimplePointLightFeatureProcessorInterface>();
    this->spotLightFp = this->scene->GetFeatureProcessor<
        AZ::Render::SimpleSpotLightFeatureProcessorInterface>();
    this->projectedShadowFp = this->scene->GetFeatureProcessor<
        AZ::Render::ProjectedShadowFeatureProcessorInterface>();
    this->meshFp = this->scene->GetFeatureProcessor<
        AZ::Render::MeshFeatureProcessorInterface>();
    std::fprintf(stderr,
        "[gz-o3de] M6 light FPs cached: dir=%p point=%p spot=%p\n",
        static_cast<void *>(this->dirLightFp),
        static_cast<void *>(this->pointLightFp),
        static_cast<void *>(this->spotLightFp));
    std::fprintf(stderr,
        "[gz-o3de] M7 shadow FPs cached: projected=%p mesh=%p\n",
        static_cast<void *>(this->projectedShadowFp),
        static_cast<void *>(this->meshFp));
    this->AcquireDemoMeshes();
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
      this->SubmitLights();
      this->SubmitMeshes();
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

  // M6: cache the 3 light FP pointers (see the interop branch above for the
  // rationale; same one-shot lookup applies on the offscreen path).
  this->dirLightFp = this->scene->GetFeatureProcessor<
      AZ::Render::DirectionalLightFeatureProcessorInterface>();
  this->pointLightFp = this->scene->GetFeatureProcessor<
      AZ::Render::SimplePointLightFeatureProcessorInterface>();
  this->spotLightFp = this->scene->GetFeatureProcessor<
      AZ::Render::SimpleSpotLightFeatureProcessorInterface>();
  this->projectedShadowFp = this->scene->GetFeatureProcessor<
      AZ::Render::ProjectedShadowFeatureProcessorInterface>();
  this->meshFp = this->scene->GetFeatureProcessor<
      AZ::Render::MeshFeatureProcessorInterface>();
  std::fprintf(stderr,
      "[gz-o3de] M6 light FPs cached: dir=%p point=%p spot=%p\n",
      static_cast<void *>(this->dirLightFp),
      static_cast<void *>(this->pointLightFp),
      static_cast<void *>(this->spotLightFp));
  std::fprintf(stderr,
      "[gz-o3de] M7 shadow FPs cached: projected=%p mesh=%p\n",
      static_cast<void *>(this->projectedShadowFp),
      static_cast<void *>(this->meshFp));
  this->AcquireDemoMeshes();

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
  // CopyWrite (UpdateImageContents upload) + ShaderWrite (-> STORAGE usage).
  // ShaderWrite/STORAGE forces NVIDIA's proprietary driver to DISABLE colour
  // (DCC) compression on this image: a DCC-compressed image shares its colour
  // plane via the exported FD but NOT the compression metadata, so the importing
  // device's sampler reads the fast-clear/metadata state (a uniform background)
  // instead of the rendered pixels -- while transfer/copy reads the real pixels
  // because the copy engine decompresses. Disabling DCC makes the colour plane
  // self-describing and the importer's sampler correct. The consumer image
  // (O3deVkImportImage) must add VK_IMAGE_USAGE_STORAGE_BIT to match (opaque-FD
  // sharing requires identical VkImageCreateInfo).
  const AZ::RHI::ImageDescriptor desc = AZ::RHI::ImageDescriptor::Create2D(
      AZ::RHI::ImageBindFlags::Color | AZ::RHI::ImageBindFlags::ShaderRead |
          AZ::RHI::ImageBindFlags::ShaderWrite |
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
    // Map a point from the shape's local frame to world (scale, then rotate,
    // then translate) -- used by the line-based primitives below.
    auto toWorld = [&](const AZ::Vector3 &_local)
    {
      return pos + rot.TransformVector(AZ::Vector3(
          _local.GetX() * scale.GetX(),
          _local.GetY() * scale.GetY(),
          _local.GetZ() * scale.GetZ()));
    };

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
      case O3deShapeData::Type::PLANE:
      {
        // A gz plane lies in its local XY plane with a +Z normal; AuxGeom's
        // DrawQuad instead builds the quad in XZ with a +Y normal. Rotate the
        // quad +90 deg about X so it matches the gz convention (normal +Z,
        // width=scale.x along X, height=scale.y along Y), then apply the visual
        // pose. FaceCullMode::None so it is visible from both sides.
        const AZ::Quaternion q =
            rot * AZ::Quaternion::CreateRotationX(AZ::Constants::HalfPi);
        const AZ::Matrix3x4 xform =
            AZ::Matrix3x4::CreateFromQuaternionAndTranslation(q, pos);
        auxGeom->DrawQuad(
            static_cast<float>(scale.GetX()), static_cast<float>(scale.GetY()),
            xform, color, AZ::RPI::AuxGeomDraw::DrawStyle::Shaded,
            AZ::RPI::AuxGeomDraw::DepthTest::On,
            AZ::RPI::AuxGeomDraw::DepthWrite::On,
            AZ::RPI::AuxGeomDraw::FaceCullMode::None);
        break;
      }
      case O3deShapeData::Type::GRID:
      {
        // A cellCount x cellCount grid of cellLength squares centred on the
        // origin in the local XY plane, plus optional stacked layers along +Z.
        const double len = shape.cellLength;
        const int n = (shape.cellCount > 0) ? shape.cellCount : 0;
        const int layers = (shape.verticalCellCount > 0)
            ? shape.verticalCellCount : 0;
        const double half = 0.5 * n * len;
        std::vector<AZ::Vector3> verts;
        verts.reserve(static_cast<size_t>((n + 1)) * 4u * (layers + 1) + 64u);
        auto pushLine = [&](const AZ::Vector3 &_a, const AZ::Vector3 &_b)
        { verts.push_back(toWorld(_a)); verts.push_back(toWorld(_b)); };
        for (int l = 0; l <= layers; ++l)
        {
          const double z = l * len;
          for (int i = 0; i <= n; ++i)
          {
            const double t = -half + i * len;
            pushLine(AZ::Vector3(-half, t, z), AZ::Vector3(half, t, z));
            pushLine(AZ::Vector3(t, -half, z), AZ::Vector3(t, half, z));
          }
        }
        if (layers > 0)  // vertical connectors between the stacked layers
        {
          for (int i = 0; i <= n; ++i)
            for (int k = 0; k <= n; ++k)
            {
              const double x = -half + i * len;
              const double y = -half + k * len;
              pushLine(AZ::Vector3(x, y, 0.0), AZ::Vector3(x, y, layers * len));
            }
        }
        if (!verts.empty())
        {
          AZ::RPI::AuxGeomDraw::AuxGeomDynamicDrawArguments args;
          args.m_verts = verts.data();
          args.m_vertCount = static_cast<uint32_t>(verts.size());
          args.m_colors = &color;
          args.m_colorCount = 1u;
          auxGeom->DrawLines(args);
        }
        break;
      }
      case O3deShapeData::Type::CAPSULE:
      {
        // gz Capsule convention: a cylinder of length capsuleLength along the
        // visual's local +Z, capped by hemispheres of radius capsuleRadius at
        // each end. AuxGeom has no native capsule, so we draw it as a body
        // cylinder + two full spheres at the ends. Depth-test occludes the
        // half of each sphere that sits inside the cylinder, which gives the
        // expected pill silhouette without explicit hemisphere geometry.
        const float r = static_cast<float>(shape.capsuleRadius);
        const float h = static_cast<float>(shape.capsuleLength);
        auxGeom->DrawCylinder(pos, axisZ, r, h, color);
        const AZ::Vector3 capOffset = axisZ * (0.5f * h);
        auxGeom->DrawSphere(pos + capOffset, r, color);
        auxGeom->DrawSphere(pos - capOffset, r, color);
        break;
      }
      case O3deShapeData::Type::FRUSTUM:
      {
        // View frustum extending along local +X (gz camera convention is +X
        // forward, +Z up, +Y left). Near rectangle at x=near, far rectangle
        // at x=far; half-extents from horizontal FoV + aspect ratio.
        // Draws the 12 edges of the prism plus the 4 apex-to-near connectors
        // so the camera position is unambiguous.
        const float nNear = static_cast<float>(shape.frustumNear);
        const float nFar  = static_cast<float>(shape.frustumFar);
        const float halfWn = nNear * std::tan(0.5f * shape.frustumHFov);
        const float halfHn = halfWn / std::max(1e-3f,
            static_cast<float>(shape.frustumAspectRatio));
        const float halfWf = nFar  * std::tan(0.5f * shape.frustumHFov);
        const float halfHf = halfWf / std::max(1e-3f,
            static_cast<float>(shape.frustumAspectRatio));
        // Frustum dimensions are absolute (encoded in near/far/hfov/aspect),
        // so apply the visual's pose only -- the visual's scale would distort
        // a real camera preview.
        auto toFrustumWorld = [&](const AZ::Vector3 &_local)
        { return pos + rot.TransformVector(_local); };
        // 8 corners in local +X-forward frame: (x, y, z) where +X = forward,
        // +Y = left, +Z = up. Indexing: near=0..3, far=4..7, each rectangle
        // ordered BL, BR, TR, TL when looking along +X from behind.
        const AZ::Vector3 cN[4] = {
            toFrustumWorld(AZ::Vector3(nNear,  halfWn, -halfHn)),
            toFrustumWorld(AZ::Vector3(nNear, -halfWn, -halfHn)),
            toFrustumWorld(AZ::Vector3(nNear, -halfWn,  halfHn)),
            toFrustumWorld(AZ::Vector3(nNear,  halfWn,  halfHn))};
        const AZ::Vector3 cF[4] = {
            toFrustumWorld(AZ::Vector3(nFar,   halfWf, -halfHf)),
            toFrustumWorld(AZ::Vector3(nFar,  -halfWf, -halfHf)),
            toFrustumWorld(AZ::Vector3(nFar,  -halfWf,  halfHf)),
            toFrustumWorld(AZ::Vector3(nFar,   halfWf,  halfHf))};
        const AZ::Vector3 apex = pos;  // visual origin
        std::vector<AZ::Vector3> verts;
        verts.reserve(32u);
        // Near rectangle (4 edges).
        for (int k = 0; k < 4; ++k)
        { verts.push_back(cN[k]); verts.push_back(cN[(k + 1) & 3]); }
        // Far rectangle (4 edges).
        for (int k = 0; k < 4; ++k)
        { verts.push_back(cF[k]); verts.push_back(cF[(k + 1) & 3]); }
        // Near->Far connectors (4 edges).
        for (int k = 0; k < 4; ++k)
        { verts.push_back(cN[k]); verts.push_back(cF[k]); }
        // Apex->near connectors (4 edges) so the camera point is visible.
        for (int k = 0; k < 4; ++k)
        { verts.push_back(apex); verts.push_back(cN[k]); }
        AZ::RPI::AuxGeomDraw::AuxGeomDynamicDrawArguments args;
        args.m_verts = verts.data();
        args.m_vertCount = static_cast<uint32_t>(verts.size());
        args.m_colors = &color;
        args.m_colorCount = 1u;
        auxGeom->DrawLines(args);
        break;
      }
      case O3deShapeData::Type::WIREBOX:
      {
        // 12 edges of the local axis-aligned box, oriented + placed by the
        // visual world pose/scale.
        const AZ::Vector3 lo(shape.boxMin[0], shape.boxMin[1], shape.boxMin[2]);
        const AZ::Vector3 hi(shape.boxMax[0], shape.boxMax[1], shape.boxMax[2]);
        const AZ::Vector3 c[8] = {
            toWorld(AZ::Vector3(lo.GetX(), lo.GetY(), lo.GetZ())),
            toWorld(AZ::Vector3(hi.GetX(), lo.GetY(), lo.GetZ())),
            toWorld(AZ::Vector3(hi.GetX(), hi.GetY(), lo.GetZ())),
            toWorld(AZ::Vector3(lo.GetX(), hi.GetY(), lo.GetZ())),
            toWorld(AZ::Vector3(lo.GetX(), lo.GetY(), hi.GetZ())),
            toWorld(AZ::Vector3(hi.GetX(), lo.GetY(), hi.GetZ())),
            toWorld(AZ::Vector3(hi.GetX(), hi.GetY(), hi.GetZ())),
            toWorld(AZ::Vector3(lo.GetX(), hi.GetY(), hi.GetZ()))};
        static const int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0},   // bottom face
            {4, 5}, {5, 6}, {6, 7}, {7, 4},   // top face
            {0, 4}, {1, 5}, {2, 6}, {3, 7}};  // verticals
        std::vector<AZ::Vector3> verts;
        verts.reserve(24u);
        for (const auto &e : edges)
        {
          verts.push_back(c[e[0]]);
          verts.push_back(c[e[1]]);
        }
        AZ::RPI::AuxGeomDraw::AuxGeomDynamicDrawArguments args;
        args.m_verts = verts.data();
        args.m_vertCount = static_cast<uint32_t>(verts.size());
        args.m_colors = &color;
        args.m_colorCount = 1u;
        auxGeom->DrawLines(args);
        break;
      }
    }
  }
}

//////////////////////////////////////////////////
// M9: build a runtime Atom ModelAsset from a gz::common::Mesh.
//
// Every triangle submesh of the source mesh is merged into a single Atom
// mesh/LOD (vertex base + index offsets adjusted) so the whole geometry renders
// under one MeshFeatureProcessor handle. Atom's mesh pipeline expects the full
// POSITION/NORMAL/TANGENT/BITANGENT/UV stream set, so any channel the source
// lacks is synthesised (a flat +X tangent / +Y bitangent frame and zero UVs) to
// keep every stream at exactly one element per vertex. Returns a Ready model on
// success, or a null asset when the mesh has no triangle geometry.
namespace
{
  // Wrap a raw CPU data buffer in a host-visible Atom BufferAsset. This mirrors
  // the private ModelAssetHelpers::CreateBufferAsset (not accessible from here):
  // a one-off InputAssembly buffer pool + a structured buffer view over a copy
  // of the data. One pool per buffer is wasteful but fine at scene-setup scale.
  AZ::Data::Asset<AZ::RPI::BufferAsset> MakeBufferAsset(
      const void *_data, uint32_t _elementCount, uint32_t _elementSize)
  {
    AZ::Data::Asset<AZ::RPI::ResourcePoolAsset> poolAsset;
    {
      const AZ::Data::AssetId poolId = AZ::Uuid::CreateRandom();
      poolAsset = AZ::Data::AssetManager::Instance().CreateAsset(
          poolId, azrtti_typeid<AZ::RPI::ResourcePoolAsset>(),
          AZ::Data::AssetLoadBehavior::PreLoad);

      auto poolDesc = AZStd::make_unique<AZ::RHI::BufferPoolDescriptor>();
      poolDesc->m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly;
      poolDesc->m_heapMemoryLevel = AZ::RHI::HeapMemoryLevel::Host;

      AZ::RPI::ResourcePoolAssetCreator creator;
      creator.Begin(poolId);
      creator.SetPoolDescriptor(AZStd::move(poolDesc));
      creator.SetPoolName("GzO3deMeshBufferPool");
      creator.End(poolAsset);
    }

    AZ::Data::Asset<AZ::RPI::BufferAsset> asset;
    {
      const AZ::Data::AssetId bufferId = AZ::Uuid::CreateRandom();
      asset = AZ::Data::AssetManager::Instance().CreateAsset(
          bufferId, azrtti_typeid<AZ::RPI::BufferAsset>(),
          AZ::Data::AssetLoadBehavior::PreLoad);

      AZ::RHI::BufferDescriptor bufferDescriptor;
      bufferDescriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly;
      bufferDescriptor.m_byteCount =
          static_cast<uint64_t>(_elementCount) * _elementSize;

      AZ::RPI::BufferAssetCreator creator;
      creator.Begin(bufferId);
      creator.SetPoolAsset(poolAsset);
      creator.SetBuffer(_data, bufferDescriptor.m_byteCount, bufferDescriptor);
      creator.SetBufferViewDescriptor(
          AZ::RHI::BufferViewDescriptor::CreateStructured(
              0, _elementCount, _elementSize));
      creator.End(asset);
    }
    return asset;
  }

  // Extract the merged, Atom-ready CPU geometry from a gz::common::Mesh. No
  // Atom/AzCore calls -- safe on the gz thread (RegisterMesh). See
  // MeshGeometryCpu + BuildModelAssetFromGeometry for the two halves of the M9
  // converter.
  MeshGeometryCpu ExtractMeshGeometry(const gz::common::Mesh *_mesh)
  {
    MeshGeometryCpu g;
    if (_mesh == nullptr || _mesh->SubMeshCount() == 0)
      return g;

    for (unsigned int s = 0; s < _mesh->SubMeshCount(); ++s)
    {
      auto subMesh = _mesh->SubMeshByIndex(s).lock();
      if (!subMesh)
        continue;
      // Only indexed triangle lists map onto Atom's indexed-triangle mesh.
      if (subMesh->SubMeshPrimitiveType() != gz::common::SubMesh::TRIANGLES)
        continue;

      const uint32_t vbase = static_cast<uint32_t>(g.positions.size() / 3);
      const unsigned int vcount = subMesh->VertexCount();
      const bool hasNormals = subMesh->NormalCount() == vcount;
      const bool hasUVs = subMesh->TexCoordCount() == vcount;

      for (unsigned int v = 0; v < vcount; ++v)
      {
        const gz::math::Vector3d p = subMesh->Vertex(v);
        g.positions.push_back(static_cast<float>(p.X()));
        g.positions.push_back(static_cast<float>(p.Y()));
        g.positions.push_back(static_cast<float>(p.Z()));

        gz::math::Vector3d n(0.0, 0.0, 1.0);
        if (hasNormals)
          n = subMesh->Normal(v);
        if (n.Length() < 1e-6)
          n = gz::math::Vector3d(0.0, 0.0, 1.0);
        n.Normalize();
        g.normals.push_back(static_cast<float>(n.X()));
        g.normals.push_back(static_cast<float>(n.Y()));
        g.normals.push_back(static_cast<float>(n.Z()));

        // Derive a VALID per-vertex tangent frame from the normal. A constant
        // tangent (the old placeholder used (1,0,0) for every vertex) is fatal:
        // on any face whose normal is parallel to it -- the +/-X faces of an
        // axis-aligned box -- the tangent/normal pair is degenerate, so
        // StandardPBR's TBN basis collapses and those faces shade solid BLACK
        // while the perpendicular faces (top, +/-Y) light correctly. That is
        // exactly the "black box with a white top" the runtime meshes showed in
        // M9-A/M9-B/M10 (it was misread as a lighting-saturation effect). The
        // cooked .azmodel floor looked fine only because it ships real baked
        // tangents. Gram-Schmidt a reference axis that is least aligned with the
        // normal so the result is always perpendicular to it.
        gz::math::Vector3d ref = (std::abs(n.X()) < 0.9)
            ? gz::math::Vector3d(1.0, 0.0, 0.0)
            : gz::math::Vector3d(0.0, 1.0, 0.0);
        gz::math::Vector3d tang = ref - n * n.Dot(ref);
        if (tang.Length() < 1e-6)
          tang = gz::math::Vector3d(0.0, 1.0, 0.0);
        tang.Normalize();
        gz::math::Vector3d bitang = n.Cross(tang);  // unit (n _|_ tang)
        g.tangents.push_back(static_cast<float>(tang.X()));
        g.tangents.push_back(static_cast<float>(tang.Y()));
        g.tangents.push_back(static_cast<float>(tang.Z()));
        g.tangents.push_back(1.0f);  // handedness: bitangent = n x tangent
        g.bitangents.push_back(static_cast<float>(bitang.X()));
        g.bitangents.push_back(static_cast<float>(bitang.Y()));
        g.bitangents.push_back(static_cast<float>(bitang.Z()));

        if (hasUVs)
        {
          const gz::math::Vector2d uv = subMesh->TexCoord(v);
          g.uvs.push_back(static_cast<float>(uv.X()));
          g.uvs.push_back(static_cast<float>(uv.Y()));
        }
        else
        {
          g.uvs.push_back(0.0f);
          g.uvs.push_back(0.0f);
        }
      }

      // gz::common triangle winding is the opposite of Atom's front-face
      // convention, so emit each triangle's indices reversed (v0, v2, v1).
      // Without this the lit exterior faces are back-face-culled and only the
      // unlit interior is visible (mesh renders black). Trailing indices that
      // do not complete a triangle are copied verbatim.
      const unsigned int icount = subMesh->IndexCount();
      for (unsigned int i = 0; i + 2 < icount; i += 3)
      {
        g.indices.push_back(vbase + static_cast<uint32_t>(subMesh->Index(i)));
        g.indices.push_back(vbase + static_cast<uint32_t>(subMesh->Index(i + 2)));
        g.indices.push_back(vbase + static_cast<uint32_t>(subMesh->Index(i + 1)));
      }
      for (unsigned int i = (icount / 3) * 3; i < icount; ++i)
      {
        g.indices.push_back(vbase + static_cast<uint32_t>(subMesh->Index(i)));
      }
    }

    return g;
  }

  // Build a Ready Atom ModelAsset from extracted CPU geometry. Atom asset
  // construction -- MUST run on the render thread (see the AssetManager note on
  // O3deBackend::Impl).
  AZ::Data::Asset<AZ::RPI::ModelAsset> BuildModelAssetFromGeometry(
      const MeshGeometryCpu &_geom, const AZ::Name &_name,
      const AZ::Data::Asset<AZ::RPI::MaterialAsset> &_defaultMaterial = {})
  {
    AZ::Data::Asset<AZ::RPI::ModelAsset> nullAsset;
    if (_geom.Empty())
      return nullAsset;

    const uint32_t vertexCount =
        static_cast<uint32_t>(_geom.positions.size() / 3);
    const uint32_t indexCount = static_cast<uint32_t>(_geom.indices.size());

    AZ::Aabb aabb = AZ::Aabb::CreateNull();
    for (size_t i = 0; i + 2 < _geom.positions.size(); i += 3)
    {
      aabb.AddPoint(AZ::Vector3(
          _geom.positions[i], _geom.positions[i + 1], _geom.positions[i + 2]));
    }

    const auto &positions = _geom.positions;
    const auto &normals = _geom.normals;
    const auto &tangents = _geom.tangents;
    const auto &bitangents = _geom.bitangents;
    const auto &uvs = _geom.uvs;
    const auto &indices = _geom.indices;

    AZ::RPI::ModelLodAssetCreator lodCreator;
    lodCreator.Begin(AZ::Uuid::CreateRandom());
    lodCreator.BeginMesh();
    lodCreator.SetMeshAabb(AZ::Aabb(aabb));
    lodCreator.SetMeshMaterialSlot(0);
    lodCreator.SetMeshIndexBuffer(
        { MakeBufferAsset(indices.data(), indexCount, sizeof(uint32_t)),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, indexCount, AZ::RHI::Format::R32_UINT) });
    lodCreator.AddMeshStreamBuffer(
        AZ::RHI::ShaderSemantic(AZ::Name("POSITION")), AZ::Name(),
        { MakeBufferAsset(positions.data(), vertexCount, sizeof(float) * 3),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, vertexCount, AZ::RHI::Format::R32G32B32_FLOAT) });
    lodCreator.AddMeshStreamBuffer(
        AZ::RHI::ShaderSemantic(AZ::Name("NORMAL")), AZ::Name(),
        { MakeBufferAsset(normals.data(), vertexCount, sizeof(float) * 3),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, vertexCount, AZ::RHI::Format::R32G32B32_FLOAT) });
    lodCreator.AddMeshStreamBuffer(
        AZ::RHI::ShaderSemantic(AZ::Name("TANGENT")), AZ::Name(),
        { MakeBufferAsset(tangents.data(), vertexCount, sizeof(float) * 4),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, vertexCount, AZ::RHI::Format::R32G32B32A32_FLOAT) });
    lodCreator.AddMeshStreamBuffer(
        AZ::RHI::ShaderSemantic(AZ::Name("BITANGENT")), AZ::Name(),
        { MakeBufferAsset(bitangents.data(), vertexCount, sizeof(float) * 3),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, vertexCount, AZ::RHI::Format::R32G32B32_FLOAT) });
    lodCreator.AddMeshStreamBuffer(
        AZ::RHI::ShaderSemantic(AZ::Name("UV")), AZ::Name(),
        { MakeBufferAsset(uvs.data(), vertexCount, sizeof(float) * 2),
          AZ::RHI::BufferViewDescriptor::CreateTyped(
              0, vertexCount, AZ::RHI::Format::R32G32_FLOAT) });
    lodCreator.EndMesh();
    AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
    if (!lodCreator.End(lodAsset))
      return nullAsset;

    AZ::RPI::ModelAssetCreator modelCreator;
    modelCreator.Begin(AZ::Uuid::CreateRandom());
    modelCreator.SetName(_name.GetStringView());
    // One material slot (stableId 0) to satisfy the mesh's SetMeshMaterialSlot(0)
    // reference. Assign a DEFAULT MATERIAL ASSET to the slot so the model matches
    // how cooked .azmodel slots (which carry their baked material) and the
    // WhiteBox gem (which builds models at runtime and lights correctly) are set
    // up -- a slot left empty is non-canonical. NOTE: this was tested as a fix
    // for the runtime-mesh-unlit bug (see SubmitMeshes) and did NOT resolve it
    // (the mesh still shades black under lighting), so it is kept only as correct
    // construction, not as the fix. The MeshHandleDescriptor override still
    // controls the final per-mesh appearance.
    AZ::RPI::ModelMaterialSlot slot;
    slot.m_stableId = 0;
    slot.m_displayName = AZ::Name("default");
    if (_defaultMaterial.GetId().IsValid())
      slot.m_defaultMaterialAsset = _defaultMaterial;
    modelCreator.AddMaterialSlot(slot);
    modelCreator.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> modelAsset;
    if (!modelCreator.End(modelAsset))
      return nullAsset;

    return modelAsset;
  }

  // Convenience for the render thread: extract + build in one call (used by the
  // M9-A demo gate, which already runs on the render thread).
  AZ::Data::Asset<AZ::RPI::ModelAsset> BuildModelAssetFromCommonMesh(
      const gz::common::Mesh *_mesh, const AZ::Name &_name)
  {
    return BuildModelAssetFromGeometry(ExtractMeshGeometry(_mesh), _name);
  }
}  // namespace

//////////////////////////////////////////////////
void O3deBackend::Impl::AcquireDemoMeshes()
{
  if (!this->meshFp)
    return;
  if (!this->demoMeshHandles.empty())
    return;  // already acquired in the other SetupScene path

  // M9-A demo gate: prove the runtime gz::common::Mesh -> Atom ModelAsset path
  // (BuildModelAssetFromCommonMesh) end to end, independently of the cooked
  // .azmodel demo below. Builds a built-in gz-common box via MeshManager,
  // converts it to an Atom model at runtime, and renders it through the same
  // MeshFeatureProcessor the cooked assets use. When this gate is set we render
  // ONLY the procedural box so the result is unambiguous to verify.
  if (std::getenv("GZ_O3DE_DEMO_PROC_MESH"))
  {
    auto *meshMgr = gz::common::MeshManager::Instance();
    const std::string boxName = "gz_o3de_proc_box";
    if (meshMgr->MeshByName(boxName) == nullptr)
      meshMgr->CreateBox(boxName, gz::math::Vector3d(1.0, 1.0, 1.0),
          gz::math::Vector2d(1.0, 1.0));
    const gz::common::Mesh *boxMesh = meshMgr->MeshByName(boxName);

    auto modelAsset = BuildModelAssetFromCommonMesh(
        boxMesh, AZ::Name("gz_o3de_proc_box"));
    if (!modelAsset.IsReady())
    {
      std::fprintf(stderr,
          "[gz-o3de] M9-A procedural box model build FAILED "
          "(submeshes=%u) -- no mesh rendered\n",
          boxMesh ? boxMesh->SubMeshCount() : 0u);
      return;
    }

    AZ::Data::Instance<AZ::RPI::Material> greyMaterial;
    auto greyAsset = AZ::RPI::AssetUtils::LoadCriticalAsset<
        AZ::RPI::MaterialAsset>("materials/basic_grey.azmaterial",
            AZ::RPI::AssetUtils::TraceLevel::Warning);
    if (greyAsset.IsReady())
      greyMaterial = AZ::RPI::Material::FindOrCreate(greyAsset);

    AZ::Render::MeshHandleDescriptor desc = greyMaterial
        ? AZ::Render::MeshHandleDescriptor(modelAsset, greyMaterial)
        : AZ::Render::MeshHandleDescriptor(modelAsset);
    auto handle = this->meshFp->AcquireMesh(desc);
    if (!handle.IsValid())
    {
      std::fprintf(stderr,
          "[gz-o3de] M9-A procedural box AcquireMesh FAILED\n");
      return;
    }
    this->meshFp->SetTransform(handle,
        AZ::Transform::CreateTranslation(AZ::Vector3(0.0f, 0.0f, 1.0f)),
        AZ::Vector3(1.0f, 1.0f, 1.0f));
    this->demoMeshHandles.emplace_back(std::move(handle));
    std::fprintf(stderr,
        "[gz-o3de] M9-A procedural box rendered: %u submesh(es) -> "
        "1 Atom model @ (0,0,1)\n",
        boxMesh->SubMeshCount());
    return;
  }

  if (!std::getenv("GZ_O3DE_DEMO_SHAPES"))
    return;

  // Two cooked assets from the existing PoC cache: a sphere (caster) and
  // an occlusion-culling-plane (receiver, repurposed as a ground plane).
  // LoadCriticalAsset blocks until the model is fully loaded -- safe here
  // because we run during scene setup, off the render thread.
  struct DemoMesh
  {
    const char *assetPath;
    AZ::Vector3 pos;
    AZ::Vector3 scale;
    const char *kind;
  };
  const DemoMesh demos[] = {
      // Ground receiver: sphere.fbx.azmodel squashed flat (scale Z=0.1) so it
      // sits at z=0 as a wide lit floor the spot's cone footprint and the
      // shadow projector land on. (The old free-floating "sphere caster" was
      // dropped in the scene reorg -- the M9 hero gz-common mesh injected via
      // MaybeInjectDemoMeshData is now the mesh showcase + shadow caster.)
      { "models/sphere.fbx.azmodel",
        AZ::Vector3(0.0f, 0.0f, 0.0f),
        AZ::Vector3(10.0f, 10.0f, 0.1f),
        "flat-sphere-receiver" },
  };

  // Load the shared mid-grey PBR material once -- the sphere asset's
  // default-baked material has a near-white albedo that washes out the
  // spot's coloured contribution AND makes any cast shadow nearly
  // invisible against the bright reflected ambient. basic_grey gives
  // us a mid-tone albedo so the spot tint + sphere shadow register.
  auto greyMaterialAsset = AZ::RPI::AssetUtils::LoadCriticalAsset<
      AZ::RPI::MaterialAsset>("materials/basic_grey.azmaterial",
          AZ::RPI::AssetUtils::TraceLevel::Warning);
  AZ::Data::Instance<AZ::RPI::Material> greyMaterial;
  if (greyMaterialAsset.IsReady())
  {
    greyMaterial = AZ::RPI::Material::FindOrCreate(greyMaterialAsset);
  }
  if (!greyMaterial)
  {
    std::fprintf(stderr,
        "[gz-o3de] M7 basic_grey material unavailable -- meshes will use "
        "their baked default material (likely near-white)\n");
  }

  for (const auto &d : demos)
  {
    auto modelAsset = AZ::RPI::AssetUtils::LoadCriticalAsset<
        AZ::RPI::ModelAsset>(d.assetPath,
            AZ::RPI::AssetUtils::TraceLevel::Warning);
    if (!modelAsset.IsReady())
    {
      std::fprintf(stderr,
          "[gz-o3de] M7 demo mesh '%s' (%s) failed to load -- skipping\n",
          d.kind, d.assetPath);
      continue;
    }
    AZ::Render::MeshHandleDescriptor desc = greyMaterial
        ? AZ::Render::MeshHandleDescriptor(modelAsset, greyMaterial)
        : AZ::Render::MeshHandleDescriptor(modelAsset);
    auto handle = this->meshFp->AcquireMesh(desc);
    if (!handle.IsValid())
    {
      std::fprintf(stderr,
          "[gz-o3de] M7 demo mesh '%s' AcquireMesh failed\n", d.kind);
      continue;
    }
    AZ::Transform xform =
        AZ::Transform::CreateTranslation(d.pos);
    this->meshFp->SetTransform(handle, xform, d.scale);
    this->demoMeshHandles.emplace_back(std::move(handle));
    std::fprintf(stderr,
        "[gz-o3de] M7 demo mesh acquired: %s @ (%.2f,%.2f,%.2f) "
        "scale (%.2f,%.2f,%.2f) (total demoMeshes=%zu)\n",
        d.kind,
        aznumeric_cast<float>(d.pos.GetX()),
        aznumeric_cast<float>(d.pos.GetY()),
        aznumeric_cast<float>(d.pos.GetZ()),
        aznumeric_cast<float>(d.scale.GetX()),
        aznumeric_cast<float>(d.scale.GetY()),
        aznumeric_cast<float>(d.scale.GetZ()),
        this->demoMeshHandles.size());
  }
}

//////////////////////////////////////////////////
void O3deBackend::Impl::SubmitMeshes()
{
  if (!this->meshFp)
    return;

  // Process pending unregistrations first: release the Atom handle and drop the
  // cached model/geometry. Done render-side so MeshFP teardown stays on-thread.
  {
    std::vector<uint64_t> toDrop;
    {
      std::lock_guard<std::mutex> lock(this->mutex);
      toDrop.swap(this->meshUnregister);
    }
    for (uint64_t id : toDrop)
    {
      auto hIt = this->meshHandles.find(id);
      if (hIt != this->meshHandles.end())
      {
        this->meshFp->ReleaseMesh(hIt->second);
        this->meshHandles.erase(hIt);
      }
      this->meshModels.erase(id);
      this->meshMaterials.erase(id);
      std::lock_guard<std::mutex> lock(this->mutex);
      this->meshGeometry.erase(id);
    }
  }

  // Nothing to draw and nothing left to release -- skip the material load.
  if (this->meshes.empty() && this->meshHandles.empty())
    return;

  // Lazily load the shared StandardPBR base material once (basic_grey). Each
  // mesh gets its own Material::Create instance below so its baseColor.color
  // can be tinted from O3deMeshData::color independently (M10).
  if (!this->meshMaterialAsset.IsReady())
  {
    this->meshMaterialAsset = AZ::RPI::AssetUtils::LoadCriticalAsset<
        AZ::RPI::MaterialAsset>("materials/basic_grey.azmaterial",
            AZ::RPI::AssetUtils::TraceLevel::Warning);
  }

  // Acquire-on-first-sight + per-frame transform for every mesh present this
  // frame; release handles whose id has gone (same lifecycle as SubmitLights).
  std::unordered_set<uint64_t> present;
  for (const O3deMeshData &m : this->meshes)
  {
    present.insert(m.id);

    auto hIt = this->meshHandles.find(m.id);
    if (hIt == this->meshHandles.end())
    {
      // Build the Atom model on first sight from the registered CPU geometry.
      auto modelIt = this->meshModels.find(m.id);
      if (modelIt == this->meshModels.end())
      {
        MeshGeometryCpu geom;
        {
          std::lock_guard<std::mutex> lock(this->mutex);
          auto gIt = this->meshGeometry.find(m.id);
          if (gIt != this->meshGeometry.end())
            geom = gIt->second;
        }
        if (geom.Empty())
          continue;  // geometry not registered (yet) for this id
        auto model = BuildModelAssetFromGeometry(geom,
            AZ::Name(AZStd::string::format("gz_mesh_%llu",
                static_cast<unsigned long long>(m.id))),
            this->meshMaterialAsset);
        if (!model.IsReady())
        {
          std::fprintf(stderr,
              "[gz-o3de] M9 mesh id=%llu model build FAILED\n",
              static_cast<unsigned long long>(m.id));
          continue;
        }
        modelIt = this->meshModels.emplace(m.id, std::move(model)).first;
      }

      // Per-mesh StandardPBR instance coloured from O3deMeshData::color.
      //
      // KNOWN LIMITATION (runtime meshes render unlit): a model built at runtime
      // by BuildModelAssetFromGeometry shades solid BLACK under scene lighting,
      // even though its construction is byte-for-byte identical to Atom's own
      // ModelAssetHelpers::CreateModel (same POSITION/NORMAL/TANGENT/BITANGENT/UV
      // streams, formats and order), its CPU normals are correct, its material
      // binds, and the mesh IS in the lit forward pass. The pixels come out
      // exactly srgb(0,0,0) across diffuse + specular + IBL while EMISSIVE still
      // shows -- i.e. the world-space vertex normal reads as zero in the shader
      // for our runtime models specifically (cooked .azmodel assets, e.g. the
      // demo floor, light correctly through the very same material + lights).
      // This was isolated by elimination -- it is not the material path
      // (FindOrCreate behaves the same), not the winding, not the tangent frame,
      // not r_enablePerMeshShaderOptionFlags, not back-face culling, not an empty
      // model material slot (assigning m_defaultMaterialAsset like WhiteBox does
      // changed nothing), and the stream construction is byte-for-byte identical
      // to Atom's own ModelAssetHelpers::CreateModel. The fix lives somewhere
      // deeper in how this minimal Atom pipeline wires per-mesh normals for
      // hand-built models.
      //
      // DECISIVE PARTITION (2026-06-03): the GZ_O3DE_MESH_DEBUG_NORMALS test below
      // proved the OBJECT-space NORMAL stream reaches the pixel shader fully
      // correct (box faces sampled as clean orthonormal axis normals: +Y front,
      // -X left, -Z bottom). So construction/streams/data are NOT the bug. The
      // DebugVertexStreams material outputs the normal UNTRANSFORMED and works;
      // StandardPBR transforms it by the per-object normal matrix and yields zero
      // lighting -> the defect is strictly in the WORLD-normal path for runtime
      // models, not in the mesh. Next diagnostic: visualize the WORLD normal (or
      // dump the ObjectSrg normal matrix) to confirm it degenerates for hand-built
      // models while the position transform stays correct.
      //
      // Until that is fixed we drive the colour through EMISSIVE so the gz::common
      // -> Atom runtime mesh is actually VISIBLE in the demo (and the M10 per-mesh
      // tint is observable). baseColor is set too, so the mesh will shade
      // correctly for free once the runtime-normal issue is resolved.
      AZ::Data::Instance<AZ::RPI::Material> material;
      // DIRECT-EVIDENCE DIAGNOSTIC (runtime-mesh-unlit): Atom's DebugVertexStreams
      // material reads `m_normal : NORMAL` UNCONDITIONALLY and outputs it as RGB
      // (normalize(n)*0.5+0.5). Applying it to the runtime mesh answered the open
      // question: the box renders COLOURED with correct per-face normals, so the
      // vertex NORMAL DOES reach the shader and the bug is downstream in the
      // world-normal/lighting path (see the partition note above). Kept as a
      // re-runnable diagnostic, gated by GZ_O3DE_MESH_DEBUG_NORMALS (off by
      // default): set it to recolour runtime meshes by their object-space normal.
      if (std::getenv("GZ_O3DE_MESH_DEBUG_NORMALS"))
      {
        auto dbgAsset = AZ::RPI::AssetUtils::LoadCriticalAsset<
            AZ::RPI::MaterialAsset>(
                "materials/special/debugvertexstreams.azmaterial",
                AZ::RPI::AssetUtils::TraceLevel::Warning);
        if (dbgAsset.IsReady())
          material = AZ::RPI::Material::FindOrCreate(dbgAsset);
        std::fprintf(stderr,
            "[gz-o3de] DBG normals-view material ready=%d instance=%d\n",
            dbgAsset.IsReady() ? 1 : 0, material ? 1 : 0);
      }
      else if (this->meshMaterialAsset.IsReady())
      {
        material = AZ::RPI::Material::Create(this->meshMaterialAsset);
        if (material)
        {
          const AZ::Color color(m.color[0], m.color[1], m.color[2], m.color[3]);
          const auto baseColorIdx =
              material->FindPropertyIndex(AZ::Name("baseColor.color"));
          if (baseColorIdx.IsValid())
            material->SetPropertyValue(baseColorIdx, color);

          // Emissive visibility workaround (see limitation note above). Gated by
          // GZ_O3DE_MESH_EMISSIVE (the live demo sets it) so it is opt-in: once
          // the runtime-mesh-unlit bug is fixed the mesh will shade from baseColor
          // + scene lights and this crutch can simply be dropped from the demo.
          // emissive.useTexture defaults to true, which would sample a (missing)
          // emissive map and wash the flat colour out -- force it false so the
          // mesh glows in its own albedo colour.
          if (std::getenv("GZ_O3DE_MESH_EMISSIVE"))
          {
            const auto enIdx =
                material->FindPropertyIndex(AZ::Name("emissive.enable"));
            const auto useTexIdx =
                material->FindPropertyIndex(AZ::Name("emissive.useTexture"));
            const auto ecIdx =
                material->FindPropertyIndex(AZ::Name("emissive.color"));
            const auto eiIdx =
                material->FindPropertyIndex(AZ::Name("emissive.intensity"));
            if (enIdx.IsValid())
              material->SetPropertyValue(enIdx, true);
            if (useTexIdx.IsValid())
              material->SetPropertyValue(useTexIdx, false);
            if (ecIdx.IsValid())
              material->SetPropertyValue(ecIdx,
                  AZ::Color(m.color[0], m.color[1], m.color[2], 1.0f));
            if (eiIdx.IsValid())
              material->SetPropertyValue(eiIdx, 3.0f);  // Ev100
          }

          const bool compileOk = material->Compile();
          std::fprintf(stderr,
              "[gz-o3de] mesh id=%llu colour=(%.2f,%.2f,%.2f) %s compile=%d\n",
              static_cast<unsigned long long>(m.id),
              m.color[0], m.color[1], m.color[2],
              std::getenv("GZ_O3DE_MESH_EMISSIVE") ? "emissive" : "lit",
              compileOk ? 1 : 0);
          this->meshMaterials[m.id] = material;
        }
        else
        {
          std::fprintf(stderr,
              "[gz-o3de] mesh id=%llu Material::Create FAILED\n",
              static_cast<unsigned long long>(m.id));
        }
      }

      AZ::Render::MeshHandleDescriptor desc = material
          ? AZ::Render::MeshHandleDescriptor(modelIt->second, material)
          : AZ::Render::MeshHandleDescriptor(modelIt->second);
      auto handle = this->meshFp->AcquireMesh(desc);
      if (!handle.IsValid())
      {
        std::fprintf(stderr,
            "[gz-o3de] M9 mesh id=%llu AcquireMesh FAILED\n",
            static_cast<unsigned long long>(m.id));
        continue;
      }
      hIt = this->meshHandles.emplace(m.id, std::move(handle)).first;
    }

    // Per-frame world transform (gz quat is w,x,y,z; AZ wants x,y,z,w).
    const AZ::Quaternion rot(
        aznumeric_cast<float>(m.quat[1]), aznumeric_cast<float>(m.quat[2]),
        aznumeric_cast<float>(m.quat[3]), aznumeric_cast<float>(m.quat[0]));
    AZ::Transform xform = AZ::Transform::CreateFromQuaternionAndTranslation(
        rot, AZ::Vector3(aznumeric_cast<float>(m.pos[0]),
            aznumeric_cast<float>(m.pos[1]),
            aznumeric_cast<float>(m.pos[2])));
    this->meshFp->SetTransform(hIt->second, xform,
        AZ::Vector3(aznumeric_cast<float>(m.scale[0]),
            aznumeric_cast<float>(m.scale[1]),
            aznumeric_cast<float>(m.scale[2])));
  }

  // Release handles for ids that disappeared from the gathered set.
  for (auto it = this->meshHandles.begin(); it != this->meshHandles.end(); )
  {
    if (present.count(it->first) == 0u)
    {
      this->meshFp->ReleaseMesh(it->second);
      this->meshModels.erase(it->first);
      this->meshMaterials.erase(it->first);
      it = this->meshHandles.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

//////////////////////////////////////////////////
void O3deBackend::Impl::SubmitLights()
{
  // Build the set of gz light ids present this frame, partitioned by type --
  // we use it both to (a) acquire+sync each one against the matching FP and
  // (b) release any handle whose id is no longer in the set.
  std::unordered_set<uint32_t> dirIds;
  std::unordered_set<uint32_t> pointIds;
  std::unordered_set<uint32_t> spotIds;

  // Diagnostic knob (see memory o3de-single-light-greys-render): when set, skip
  // ALL spot shadow setup -- both the SimpleSpotLight self-shadow enable and the
  // paired ProjectedShadowFP. Lets us run "spot only, no shadow" to separate a
  // light-count effect from a shadow-setup effect. Read once.
  static const bool noShadow = (std::getenv("GZ_O3DE_DEMO_NO_SHADOW") != nullptr);

  for (const O3deLightData &light : this->lights)
  {
    const AZ::Vector3 pos = GzVec(light.pos);
    const AZ::Quaternion rot = GzQuat(light.quat);
    const AZ::Vector3 dir = AZ::Vector3(
        aznumeric_cast<float>(light.dir[0]),
        aznumeric_cast<float>(light.dir[1]),
        aznumeric_cast<float>(light.dir[2]));
    const float i = aznumeric_cast<float>(light.intensity);
    // gz light intensity is dimensionless; gz callers typically pass values
    // around 1.0 (the default in BaseLight). Scale the diffuse colour by it
    // and pass straight through to PhotometricColor -- the user is expected
    // to pre-scale into the appropriate unit (lux for directional, candela
    // for point/spot) when realistic photometry matters.
    const AZ::Color rgb(
        aznumeric_cast<float>(light.diffuseColor[0]) * i,
        aznumeric_cast<float>(light.diffuseColor[1]) * i,
        aznumeric_cast<float>(light.diffuseColor[2]) * i,
        1.0f);

    switch (light.type)
    {
      case O3deLightData::Type::DIRECTIONAL:
      {
        if (!this->dirLightFp)
          break;
        dirIds.insert(light.id);
        auto it = this->dirLightHandles.find(light.id);
        if (it == this->dirLightHandles.end())
        {
          it = this->dirLightHandles.emplace(
              light.id, this->dirLightFp->AcquireLight()).first;
          std::fprintf(stderr,
              "[gz-o3de] M6 dir light acquired: id=0x%X (total dir=%zu)\n",
              light.id, this->dirLightHandles.size());
        }
        this->dirLightFp->SetRgbIntensity(it->second,
            AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Lux>(rgb));
        this->dirLightFp->SetDirection(it->second, dir);
        break;
      }
      case O3deLightData::Type::POINT:
      {
        if (!this->pointLightFp)
          break;
        pointIds.insert(light.id);
        auto it = this->pointLightHandles.find(light.id);
        if (it == this->pointLightHandles.end())
        {
          it = this->pointLightHandles.emplace(
              light.id, this->pointLightFp->AcquireLight()).first;
          std::fprintf(stderr,
              "[gz-o3de] M6 point light acquired: id=0x%X (total point=%zu)\n",
              light.id, this->pointLightHandles.size());
        }
        this->pointLightFp->SetRgbIntensity(it->second,
            AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Candela>(
                rgb));
        this->pointLightFp->SetPosition(it->second, pos);
        this->pointLightFp->SetAttenuationRadius(it->second,
            aznumeric_cast<float>(light.attenRange));
        break;
      }
      case O3deLightData::Type::SPOT:
      {
        if (!this->spotLightFp)
          break;
        spotIds.insert(light.id);
        auto it = this->spotLightHandles.find(light.id);
        if (it == this->spotLightHandles.end())
        {
          it = this->spotLightHandles.emplace(
              light.id, this->spotLightFp->AcquireLight()).first;
          std::fprintf(stderr,
              "[gz-o3de] M6 spot light acquired: id=0x%X (total spot=%zu)\n",
              light.id, this->spotLightHandles.size());
          // M7 Phase A3: enable shadows on the spot itself. The SimpleSpot
          // FP has its own self-contained shadow path (depth map allocated
          // automatically on enable); the separate ProjectedShadowFP we
          // wire in Phase A2 is a parallel projector system used elsewhere
          // (decals, static projected shadows). Tune for the demo: 1024^2
          // atlas + PCF filtering for soft edges.
          if (!noShadow)
          {
            this->spotLightFp->SetShadowsEnabled(it->second, true);
            this->spotLightFp->SetShadowmapMaxResolution(it->second,
                AZ::Render::ShadowmapSize::Size1024);
            this->spotLightFp->SetShadowFilterMethod(it->second,
                AZ::Render::ShadowFilterMethod::Pcf);
            this->spotLightFp->SetFilteringSampleCount(it->second, 16);
            std::fprintf(stderr,
                "[gz-o3de] M7 spot shadows enabled on LightHandle: id=0x%X\n",
                light.id);
          }
        }
        this->spotLightFp->SetRgbIntensity(it->second,
            AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Candela>(
                rgb));
        // SimpleSpotLight takes a full transform; gz puts the spot down its
        // local -Z by convention. Use the quaternion the gz wrapper supplied.
        this->spotLightFp->SetTransform(it->second,
            AZ::Transform::CreateFromQuaternionAndTranslation(rot, pos));
        this->spotLightFp->SetAttenuationRadius(it->second,
            aznumeric_cast<float>(light.attenRange));
        this->spotLightFp->SetConeAngles(it->second,
            aznumeric_cast<float>(light.innerAngle),
            aznumeric_cast<float>(light.outerAngle));

        // M7 Phase A2: paired projected shadow. SimpleSpotLight has no
        // shadow path of its own -- the ProjectedShadowFP renders the depth
        // map separately and the lighting pass samples it via the light's
        // world transform. We acquire one ShadowId per spot id and update
        // its descriptor every frame from the same source-of-truth pose.
        if (this->projectedShadowFp && !noShadow)
        {
          auto sh = this->spotShadowHandles.find(light.id);
          if (sh == this->spotShadowHandles.end())
          {
            sh = this->spotShadowHandles.emplace(
                light.id,
                this->projectedShadowFp->AcquireShadow()).first;
            std::fprintf(stderr,
                "[gz-o3de] M7 spot shadow acquired: id=0x%X "
                "(total spotShadow=%zu)\n",
                light.id, this->spotShadowHandles.size());
            // One-time per-shadow setup: cap atlas size at 1024^2 (plenty
            // for the demo and avoids surprise memory pressure).
            this->projectedShadowFp->SetShadowmapMaxResolution(sh->second,
                AZ::Render::ShadowmapSize::Size1024);
          }
          AZ::Render::ProjectedShadowFeatureProcessorInterface::
              ProjectedShadowDescriptor desc;
          desc.m_transform =
              AZ::Transform::CreateFromQuaternionAndTranslation(rot, pos);
          desc.m_nearPlaneDistance = 0.1f;
          desc.m_farPlaneDistance =
              aznumeric_cast<float>(light.attenRange);
          desc.m_aspectRatio = 1.0f;
          // FOV-Y = full outer cone (the API takes the full angle, not
          // the half-angle).
          desc.m_fieldOfViewYRadians =
              2.0f * aznumeric_cast<float>(light.outerAngle);
          desc.m_isStatic = false;
          this->projectedShadowFp->SetShadowProperties(sh->second, desc);
        }
        break;
      }
    }
  }

  // Release handles whose ids are no longer in the gathered set.
  auto releaseStale = [](auto &_handles,
      const std::unordered_set<uint32_t> &_keep, auto *_fp, const char *_kind)
  {
    if (!_fp)
    {
      _handles.clear();
      return;
    }
    for (auto it = _handles.begin(); it != _handles.end();)
    {
      if (_keep.find(it->first) == _keep.end())
      {
        auto handle = it->second;
        std::fprintf(stderr,
            "[gz-o3de] M6 %s light released: id=0x%X (remaining=%zu)\n",
            _kind, it->first, _handles.size() - 1u);
        _fp->ReleaseLight(handle);
        it = _handles.erase(it);
      }
      else
      {
        ++it;
      }
    }
  };
  releaseStale(this->dirLightHandles, dirIds, this->dirLightFp, "dir");
  releaseStale(this->pointLightHandles, pointIds, this->pointLightFp, "point");
  releaseStale(this->spotLightHandles, spotIds, this->spotLightFp, "spot");

  // M7 Phase A2: spot shadows mirror spot lights. Same id key, paired
  // lifetime -- release the ShadowId whenever the matching LightHandle
  // disappears so the depth-map atlas slot is freed.
  if (this->projectedShadowFp)
  {
    for (auto it = this->spotShadowHandles.begin();
         it != this->spotShadowHandles.end();)
    {
      if (spotIds.find(it->first) == spotIds.end())
      {
        std::fprintf(stderr,
            "[gz-o3de] M7 spot shadow released: id=0x%X (remaining=%zu)\n",
            it->first, this->spotShadowHandles.size() - 1u);
        this->projectedShadowFp->ReleaseShadow(it->second);
        it = this->spotShadowHandles.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }
  else
  {
    this->spotShadowHandles.clear();
  }

  // Rate-limited per-frame summary so we can confirm the per-frame Set*Data
  // sync is actually running with the expected counts (without flooding the
  // log -- one line every ~120 frames, roughly 2-6 s of demo wall time).
  static thread_local uint64_t submitLightsTick = 0u;
  if ((submitLightsTick++ % 120u) == 0u)
  {
    std::fprintf(stderr,
        "[gz-o3de] M6 SubmitLights tick=%lu: dir=%zu point=%zu spot=%zu "
        "(gathered=%zu)\n",
        static_cast<unsigned long>(submitLightsTick),
        this->dirLightHandles.size(), this->pointLightHandles.size(),
        this->spotLightHandles.size(), this->lights.size());
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
        this->lights = this->pendingLights;
        this->meshes = this->pendingMeshes;
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
  //
  // `--console-mode` is what suppresses Atom's parasitic native X11 window.
  // Without it, GameApplication's QueryApplicationType reports Game (not
  // ConsoleMode), so Atom's BootstrapSystemComponent::Activate() takes the
  // "create the game window" branch (vendor/o3de/Gems/Atom/Bootstrap/Code/
  // Source/BootstrapSystemComponent.cpp:358) -- it builds a 1920x1080
  // AzFramework::NativeWindow titled "GzAtomPoc" that the WM then stacks on
  // top of gz-gui's window, fully grey because Atom's render-to-texture
  // pipeline never draws into that swapchain. With `--console-mode` Atom
  // takes the IsConsoleMode branch (line 349) which sets m_nativeWindow =
  // nullptr and additionally runs the BRDF pipeline for render-to-texture
  // (line 475) -- exactly what we want for the o3de PoC. Pair with
  // app->SetConsoleModeSupported(true) below; both are required.
  static char arg0[] = "gz-rendering-o3de";
  static char arg1[] = "--rhi=vulkan";
  static char arg2[] = "--console-mode";
  static char *fakeArgv[] = { arg0, arg1, arg2, nullptr };
  int fakeArgc = 3;

  this->app = new AzGameFramework::GameApplication(
      fakeArgc, fakeArgv, AZStd::move(componentAppSettings));

  // Required for `--console-mode` to take effect; GameApplication's
  // QueryApplicationType ANDs against m_consoleModeSupported.
  this->app->SetConsoleModeSupported(true);

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
  // GZ_O3DE_DEMO_SHAPES is a demo-only flag that stands in for gz-sim content.
  // Inject regardless of what GatherFrame already collected: gz-gui plugins such
  // as InteractiveViewControl add their own visuals (e.g. the orbit reference
  // sphere) the moment the user interacts, and an "_shapes.empty()" gate would
  // then suppress the whole demo as soon as one such visual appears -- which is
  // exactly the "everything vanishes when I orbit/zoom" symptom.
  if (!std::getenv("GZ_O3DE_DEMO_SHAPES"))
    return;

  // Seconds since the first call -- drives the simple per-frame animation
  // below so the live demo proves it really is a live render and not one
  // captured frame on a Qt swapchain. Set GZ_O3DE_DEMO_ANIMATE=0 to freeze.
  const char *animEnv = std::getenv("GZ_O3DE_DEMO_ANIMATE");
  const bool animate = !(animEnv && animEnv[0] == '0');
  static const auto t0 = std::chrono::steady_clock::now();
  const double t = animate
      ? std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count()
      : 0.0;
  // gz frame: +Z up, +X into the scene (away from the camera), +Y left.
  // Scene reorg: a clean front-facing row on the lit floor, the world origin
  // left CLEAR for an enlarged coordinate gnomon (the old gnomon was buried
  // inside the origin sphere + cage), the real-mesh centrepiece behind it
  // (MaybeInjectDemoMeshData), a wide backdrop wall, a frustum widget up high,
  // and a small satellite orbiting in its own clear lane above the row. Every
  // primitive animates IN PLACE (spin / bob / tilt) -- nothing sweeps through
  // another element the way the old orbiting cylinder did.
  const double spin = t * 0.6;
  const double spinQw = std::cos(spin * 0.5);
  const double spinQz = std::sin(spin * 0.5);
  const double bobZ = 0.6 + 0.22 * std::sin(t * 1.6);
  const double cylSpin = t * 0.9;
  const double cylQw = std::cos(cylSpin * 0.5);
  const double cylQz = std::sin(cylSpin * 0.5);

  // Red box -- far left, spinning about +Z in place.
  O3deShapeData box;
  box.type = O3deShapeData::Type::BOX;
  box.pos[0] = 0.0; box.pos[1] = 2.6; box.pos[2] = 0.5;
  box.quat[0] = spinQw; box.quat[1] = 0.0;
  box.quat[2] = 0.0;    box.quat[3] = spinQz;
  box.color[0] = 0.9f; box.color[1] = 0.15f; box.color[2] = 0.15f;
  _shapes.push_back(box);

  // Green sphere -- left of centre, bobbing in place.
  O3deShapeData sphere;
  sphere.type = O3deShapeData::Type::SPHERE;
  sphere.pos[0] = 0.0; sphere.pos[1] = 1.3; sphere.pos[2] = bobZ;
  sphere.color[0] = 0.15f; sphere.color[1] = 0.85f; sphere.color[2] = 0.2f;
  _shapes.push_back(sphere);

  // Yellow wire cage tracking the bobbing sphere (local AABB +/-0.55).
  O3deShapeData wireBox;
  wireBox.type = O3deShapeData::Type::WIREBOX;
  wireBox.pos[0] = 0.0; wireBox.pos[1] = 1.3; wireBox.pos[2] = bobZ;
  wireBox.boxMin[0] = -0.55; wireBox.boxMin[1] = -0.55; wireBox.boxMin[2] = -0.55;
  wireBox.boxMax[0] = 0.55;  wireBox.boxMax[1] = 0.55;  wireBox.boxMax[2] = 0.55;
  wireBox.color[0] = 1.0f; wireBox.color[1] = 0.85f; wireBox.color[2] = 0.0f;
  _shapes.push_back(wireBox);

  // Blue cylinder -- right of centre, spinning about +Z IN PLACE. (It used to
  // orbit the origin at r=1.5 and sweep straight through the box + gnomon.)
  O3deShapeData cylinder;
  cylinder.type = O3deShapeData::Type::CYLINDER;
  cylinder.pos[0] = 0.0; cylinder.pos[1] = -1.3; cylinder.pos[2] = 0.7;
  cylinder.quat[0] = cylQw; cylinder.quat[1] = 0.0;
  cylinder.quat[2] = 0.0;   cylinder.quat[3] = cylQz;
  cylinder.scale[2] = 1.4;
  cylinder.color[0] = 0.15f; cylinder.color[1] = 0.3f; cylinder.color[2] = 1.0f;
  _shapes.push_back(cylinder);

  // Magenta capsule -- far right, slowly tilting about local Y.
  const double capTilt = 0.25 * std::sin(t * 0.6);  // ~+/-0.25 rad
  O3deShapeData capsule;
  capsule.type = O3deShapeData::Type::CAPSULE;
  capsule.pos[0] = 0.0; capsule.pos[1] = -2.6; capsule.pos[2] = 0.75;
  capsule.quat[0] = std::cos(capTilt); capsule.quat[1] = 0.0;
  capsule.quat[2] = std::sin(capTilt); capsule.quat[3] = 0.0;
  capsule.capsuleRadius = 0.35;
  capsule.capsuleLength = 0.8;
  capsule.color[0] = 1.0f; capsule.color[1] = 0.1f; capsule.color[2] = 1.0f;
  _shapes.push_back(capsule);

  // Small white satellite orbiting high above the hero mesh (1.5, 0) at z=1.8,
  // r=1.3 -- a live circular motion that stays in a clear lane above every
  // other element, so it never overlaps them.
  const double satA = t * 0.8;
  O3deShapeData satellite;
  satellite.type = O3deShapeData::Type::SPHERE;
  satellite.pos[0] = 1.5 + 1.3 * std::cos(satA);
  satellite.pos[1] = 1.3 * std::sin(satA);
  satellite.pos[2] = 1.8;
  satellite.scale[0] = 0.28; satellite.scale[1] = 0.28; satellite.scale[2] = 0.28;
  satellite.color[0] = 1.0f; satellite.color[1] = 1.0f; satellite.color[2] = 0.9f;
  _shapes.push_back(satellite);

  // Ground grid the scene sits on (24x24 unit cells, centred on the origin).
  O3deShapeData grid;
  grid.type = O3deShapeData::Type::GRID;
  grid.cellCount = 24;
  grid.cellLength = 1.0;
  grid.color[0] = 0.35f; grid.color[1] = 0.35f; grid.color[2] = 0.4f;
  _shapes.push_back(grid);

  // Axis indicator at the world ORIGIN (M5 Phase C): three arrow-shaped
  // cylinder/cone pairs along +X (red), +Y (green), +Z (blue). The reorg
  // cleared the origin (the sphere + cage moved to +Y), so the gnomon is now
  // unoccluded -- and enlarged (shaft 0.7, fatter head) to read as a proper
  // coordinate landmark instead of vanishing inside the other shapes.
  static const float axisShaftLen = 0.7f;
  static const float axisShaftRad = 0.05f;
  static const float axisHeadLen  = 0.22f;
  static const float axisHeadRad  = 0.13f;
  struct AxisDef {
    double shaftPos[3];
    double tipPos[3];
    double quat[4];  // rotates local +Z -> the desired axis direction
    float color[3];
  };
  // gz quat convention: w,x,y,z. Local cylinder/cone axis is +Z.
  const double s2 = 0.70710678;  // sqrt(2)/2
  const AxisDef axes[3] = {
      // +X (red): rotate +Z by 90 deg about +Y -> +X
      {{0.5 * axisShaftLen, 0.0, 0.0},
       {axisShaftLen + 0.5 * axisHeadLen, 0.0, 0.0},
       {s2, 0.0, s2, 0.0},
       {1.0f, 0.15f, 0.15f}},
      // +Y (green): rotate +Z by -90 deg about +X -> +Y
      {{0.0, 0.5 * axisShaftLen, 0.0},
       {0.0, axisShaftLen + 0.5 * axisHeadLen, 0.0},
       {s2, -s2, 0.0, 0.0},
       {0.15f, 1.0f, 0.15f}},
      // +Z (blue): identity
      {{0.0, 0.0, 0.5 * axisShaftLen},
       {0.0, 0.0, axisShaftLen + 0.5 * axisHeadLen},
       {1.0, 0.0, 0.0, 0.0},
       {0.15f, 0.4f, 1.0f}}};
  for (const auto &a : axes)
  {
    O3deShapeData shaft;
    shaft.type = O3deShapeData::Type::CYLINDER;
    shaft.pos[0] = a.shaftPos[0]; shaft.pos[1] = a.shaftPos[1];
    shaft.pos[2] = a.shaftPos[2];
    shaft.quat[0] = a.quat[0]; shaft.quat[1] = a.quat[1];
    shaft.quat[2] = a.quat[2]; shaft.quat[3] = a.quat[3];
    shaft.scale[0] = 2.0 * axisShaftRad;
    shaft.scale[1] = 2.0 * axisShaftRad;
    shaft.scale[2] = axisShaftLen;
    shaft.color[0] = a.color[0]; shaft.color[1] = a.color[1];
    shaft.color[2] = a.color[2];
    _shapes.push_back(shaft);

    O3deShapeData head;
    head.type = O3deShapeData::Type::CONE;
    head.pos[0] = a.tipPos[0]; head.pos[1] = a.tipPos[1];
    head.pos[2] = a.tipPos[2];
    head.quat[0] = a.quat[0]; head.quat[1] = a.quat[1];
    head.quat[2] = a.quat[2]; head.quat[3] = a.quat[3];
    head.scale[0] = 2.0 * axisHeadRad;
    head.scale[1] = 2.0 * axisHeadRad;
    head.scale[2] = axisHeadLen;
    head.color[0] = a.color[0]; head.color[1] = a.color[1];
    head.color[2] = a.color[2];
    _shapes.push_back(head);
  }

  // Cyan frustum widget (M5 Phase D): a small view frustum pointing along
  // +X (gz camera convention), parked high in the front-RIGHT corner (balancing
  // the spinning box high on the left) and well clear of the row so it never
  // occludes another shape. Models e.g. a camera/sensor preview the gz-gui
  // frustum visual would draw for a child camera.
  O3deShapeData frustum;
  frustum.type = O3deShapeData::Type::FRUSTUM;
  frustum.pos[0] = -1.5; frustum.pos[1] = -2.4; frustum.pos[2] = 2.0;
  frustum.quat[0] = 1.0; frustum.quat[1] = 0.0;
  frustum.quat[2] = 0.0; frustum.quat[3] = 0.0;
  frustum.frustumNear = 0.15;
  frustum.frustumFar = 1.4;
  frustum.frustumHFov = 1.0;
  frustum.frustumAspectRatio = 1.6;
  frustum.color[0] = 0.2f; frustum.color[1] = 0.7f;
  frustum.color[2] = 1.0f; frustum.color[3] = 1.0f;
  _shapes.push_back(frustum);

  // Orange backdrop wall standing behind the whole scene (rotated -90 deg about
  // Y so the default XY quad stands vertical; widened to 7x4 so it reads as a
  // proper backdrop the row + mesh are staged against, normal facing camera).
  O3deShapeData plane;
  plane.type = O3deShapeData::Type::PLANE;
  plane.pos[0] = 3.2; plane.pos[1] = 0.0; plane.pos[2] = 1.8;
  plane.quat[0] = 0.70710678; plane.quat[1] = 0.0;
  plane.quat[2] = -0.70710678; plane.quat[3] = 0.0;
  plane.scale[0] = 4.0; plane.scale[1] = 7.0; plane.scale[2] = 1.0;
  plane.color[0] = 0.85f; plane.color[1] = 0.45f; plane.color[2] = 0.15f;
  _shapes.push_back(plane);
}

// Demo aid (M6 Phase C): when GZ_O3DE_DEMO_SHAPES is set and the caller did
// not gather any lights, inject a point+spot pair so SubmitLights() exercises
// the Acquire / SetRgbIntensity / Set{Position,Transform,...} / Release paths
// against real Atom FPs. Stable ids in the 0xD000x range so handles persist
// across frames (no acquire/release thrash) and don't collide with real gz
// light ids. Directional is skipped: its FP is intentionally not enabled (see
// SetupScene comment) and turning it on grey-screens the demo.
static void MaybeInjectDemoLights(std::vector<O3deLightData> &_lights)
{
  if (!_lights.empty() || !std::getenv("GZ_O3DE_DEMO_SHAPES"))
    return;

  // Point light: warm white, 4 m above the origin so it lights the bobbing
  // sphere column and the orbiting cylinder. Intensity in candela
  // (SimplePointLightFP unit). Bumped to 800 cd (from 200) after manual
  // verification: at 200 cd the warm tint was invisible against Atom's
  // default IBL ambient; 800 cd makes the upward-facing surfaces (top of
  // the box, the white sphere caster) noticeably warmer.
  // Scene reorg: dropped 800 -> 300 cd. At 800 the lit PBR surfaces (the floor
  // receiver + the M9 hero mesh) blew out to white and hid their albedo; 300 cd
  // keeps the warm key light visible while letting the mesh colour read. (The
  // AuxGeom primitives are unlit constant-colour, so they are unaffected.)
  O3deLightData point;
  point.type = O3deLightData::Type::POINT;
  point.id = 0xD0001u;
  point.pos[0] = 0.0; point.pos[1] = 0.0; point.pos[2] = 4.5;
  point.diffuseColor[0] = 1.0; point.diffuseColor[1] = 0.85;
  point.diffuseColor[2] = 0.7;
  point.intensity = 300.0;  // candela
  point.attenRange = 14.0;
  _lights.push_back(point);

  // Spot light: cool blue, mounted high to the upper-left back of the scene
  // at (-3, 3, 4) and AIMED at the world origin so the cone footprint lands
  // squarely on the demo grid + meshes. Atom's SimpleSpotLight shines down
  // local -Z, so the quaternion below rotates (0,0,-1) onto the unit vector
  // (3,-3,-4)/sqrt(34) ~= (0.514, -0.514, -0.686). gz quat order is (w,x,y,z);
  // GzQuat() in SubmitLights just reorders to AZ's (x,y,z,w) without any
  // axis remap, so these literals ARE the Atom quaternion. After manual
  // verification at quat=identity (which made the cone shine straight down
  // off-frame) the spot was invisible; aiming + 5x intensity bump make it
  // clear.
  O3deLightData spot;
  spot.type = O3deLightData::Type::SPOT;
  spot.id = 0xD0002u;
  spot.pos[0] = -3.0; spot.pos[1] = 3.0; spot.pos[2] = 4.0;
  spot.quat[0] = 0.918;   // w   (= cos(angle/2),     angle ~= 0.815 rad)
  spot.quat[1] = -0.281;  // x   (axis x = -1/sqrt(2))
  spot.quat[2] = -0.281;  // y   (axis y = -1/sqrt(2))
  spot.quat[3] = 0.0;     // z
  spot.diffuseColor[0] = 0.3; spot.diffuseColor[1] = 0.5;
  spot.diffuseColor[2] = 1.0;
  spot.intensity = 1500.0;  // candela (was 300; spot footprint was invisible)
  spot.attenRange = 10.0;
  spot.innerAngle = 0.35;   // ~20 deg
  spot.outerAngle = 0.7;    // ~40 deg (wider cone, clearer falloff)
  _lights.push_back(spot);

  // Scene reorg: front FILL point light on the camera side (-X). Placed LOW
  // (z~1.8, near the row's mid-height) and in front so its direction to the
  // shapes is mostly HORIZONTAL -- that is what rakes the camera-facing VERTICAL
  // side faces of the PBR meshes (the spinning hero mesh + the row). The
  // overhead warm key and the upper-left spot only light upward-facing surfaces,
  // so without this fill the hero mesh's vertical faces stay near-black and it
  // reads as a solid silhouette (the surrounding AuxGeom primitives are unlit
  // constant-colour, so they masked the problem). A first attempt put the fill
  // high (z=3) -- that over-lit the horizontal FLOOR to near-white but left the
  // vertical faces dark, proving the issue is the light's HEIGHT, not its power.
  // Cool tint + modest candela so the floor right under it doesn't hotspot.
  O3deLightData fill;
  fill.type = O3deLightData::Type::POINT;
  fill.id = 0xD0004u;
  fill.pos[0] = -4.5; fill.pos[1] = 0.0; fill.pos[2] = 1.8;
  fill.diffuseColor[0] = 0.75; fill.diffuseColor[1] = 0.85;
  fill.diffuseColor[2] = 1.0;
  fill.intensity = 700.0;  // candela
  fill.attenRange = 16.0;
  _lights.push_back(fill);

  // HERO-BOX RELIGHT (always on): RenderDoc proved the runtime hero box at
  // (1.5,0,0.7) shades CORRECTLY (cyan albedo, valid unit normals, lit up-faces)
  // but its camera-visible VERTICAL faces point ~+X/-Y/down -- away from all the
  // scene lights (overhead +Z key, -X fill) and into the dark lower IBL hemisphere,
  // so they read black. These two dedicated fills make those faces catch a
  // near-horizontal rake. This REPLACES the old GZ_O3DE_MESH_EMISSIVE crutch (the
  // demo no longer sets it): the box now shades from baseColor + scene lights like
  // every other primitive. Two intensities were probed and rejected on the way
  // here: a RING of 3 fills (lit the box but washed the cyan to flat white), and a
  // SINGLE fill (shaded nicely but strobed black whenever the spin rotated the lit
  // face away). The form below -- two OPPOSED fills, brighter camera-side key +
  // dimmer far-side fill -- keeps the box cyan-shaded at every spin angle, never
  // fully black. Kept env-overridable via GZ_O3DE_DEMO_NO_BOXFILL=1 (escape hatch).
  if (!std::getenv("GZ_O3DE_DEMO_NO_BOXFILL"))
  {
    // Two OPPOSED fills, not a ring and not a single light. A single light
    // strobes the spinning box black whenever its camera-facing face rotates
    // away; a full ring lights every face equally and washes the cyan to flat
    // white. Two opposed lights -- a brighter camera-side KEY and a dimmer
    // far-side FILL -- mean that as a face rotates out of the key it rotates
    // into the fill (so it never goes fully black), while the key>fill gradient
    // keeps a visible light->dark falloff across the cube (3D shading).
    struct { double x, y, z, cd; uint64_t id; } fills[] = {
        // camera-side key (camera ~(-4,0,1.2) looks +X): low, offset -Y, raking
        { -0.8, -1.4, 1.7, 120.0, 0xD0005u},
        // far-side fill (+X/+Y), dimmer -> lifts the away-faces off pure black
        {  3.6,  1.5, 1.5,  55.0, 0xD0006u},
    };
    for (const auto &l : fills)
    {
      O3deLightData f;
      f.type = O3deLightData::Type::POINT;
      f.id = l.id;
      f.pos[0] = l.x; f.pos[1] = l.y; f.pos[2] = l.z;
      f.diffuseColor[0] = 0.85; f.diffuseColor[1] = 0.92; f.diffuseColor[2] = 1.0;
      f.intensity = l.cd;
      f.attenRange = 9.0;
      _lights.push_back(f);
    }
    std::fprintf(stderr, "[gz-o3de] hero-box relight: opposed key+fill (120/55 cd)\n");
  }

  // M8: directional "sun" light, gated by the same GZ_O3DE_ENABLE_DIRECTIONAL
  // flag that registers the FP in SetupScene. Without the FP registered,
  // SubmitLights short-circuits the DIRECTIONAL branch, so this instance is
  // inert unless the flag is set. Aimed mostly down and tilted toward +X so it
  // grazes the demo shapes. DirectionalLightFP intensity unit is lux.
  if (std::getenv("GZ_O3DE_ENABLE_DIRECTIONAL"))
  {
    O3deLightData sun;
    sun.type = O3deLightData::Type::DIRECTIONAL;
    sun.id = 0xD0003u;
    // Normalized (0.286, 0, -0.958): down with a forward tilt.
    sun.dir[0] = 0.286; sun.dir[1] = 0.0; sun.dir[2] = -0.958;
    sun.diffuseColor[0] = 1.0; sun.diffuseColor[1] = 0.98;
    sun.diffuseColor[2] = 0.95;
    sun.intensity = 5.0;  // lux (modest; tune up if washed out by IBL)
    _lights.push_back(sun);
    std::fprintf(stderr, "[gz-o3de] M8: directional sun injected (id=0xD0003)\n");
  }

  // Diagnostic single-variable knob for the "single-light greys the whole
  // render" investigation (see memory o3de-single-light-greys-render). When
  // GZ_O3DE_DEMO_ONLY_LIGHT is set, drop one of the pair so SubmitLights runs
  // with exactly one light. "point" keeps the point only (no spot => no shadow
  // pass at all); "spot" keeps the spot only (shadow pass with a single light).
  // Comparing the two, plus GZ_O3DE_DEMO_NO_SHADOW below, isolates whether the
  // grey is a light-count effect or a shadow-setup effect.
  if (const char *only = std::getenv("GZ_O3DE_DEMO_ONLY_LIGHT"))
  {
    const O3deLightData::Type keep = (std::string(only) == "spot")
        ? O3deLightData::Type::SPOT : O3deLightData::Type::POINT;
    _lights.erase(std::remove_if(_lights.begin(), _lights.end(),
        [keep](const O3deLightData &l){ return l.type != keep; }),
        _lights.end());
    std::fprintf(stderr,
        "[gz-o3de] DEMO_ONLY_LIGHT=%s -> %zu demo light(s)\n",
        only, _lights.size());
  }
}

// Demo aid (M9-B / M10 / demo reorg): exercise the FULL public-mesh pipeline
// end to end without gz-sim, AND give the GZ_O3DE_DEMO_SHAPES scene a real
// gz::common mesh centrepiece now that mesh support exists. Fires when EITHER
// GZ_O3DE_DEMO_MESH_PIPE (the original M9-B pipeline probe) or GZ_O3DE_DEMO_SHAPES
// (the live demo) is set: register a built-in gz-common box once (gz-thread
// geometry extraction via RegisterMesh) and inject one O3deMeshData per frame --
// so the snapshot crosses the thread boundary and the render thread builds the
// model, acquires the MeshFP handle, and updates the transform exactly as it
// would for a real gz-sim mesh visual gathered in O3deRenderTarget. The id is
// in the 0xD009x demo range. Runs on the caller's (gz) thread.
static void MaybeInjectDemoMeshData(std::vector<O3deMeshData> &_meshes)
{
  if (!std::getenv("GZ_O3DE_DEMO_MESH_PIPE") &&
      !std::getenv("GZ_O3DE_DEMO_SHAPES"))
    return;

  constexpr uint64_t kDemoMeshId = 0xD0091u;

  // Register the geometry exactly once (RegisterMesh is idempotent-safe but the
  // extraction is wasted work every frame otherwise).
  static bool registered = false;
  if (!registered)
  {
    auto *meshMgr = gz::common::MeshManager::Instance();
    const std::string boxName = "gz_o3de_pipe_box";
    if (meshMgr->MeshByName(boxName) == nullptr)
      meshMgr->CreateBox(boxName, gz::math::Vector3d(1.0, 1.0, 1.0),
          gz::math::Vector2d(1.0, 1.0));
    O3deBackend::Instance().RegisterMesh(
        kDemoMeshId, meshMgr->MeshByName(boxName));
    registered = true;
    std::fprintf(stderr,
        "[gz-o3de] M9-B DEMO_MESH_PIPE: registered demo box id=0x%llx\n",
        static_cast<unsigned long long>(kDemoMeshId));
  }

  // Hero mesh: the real gz::common box, staged behind the now-clear world origin
  // as the centrepiece of the reorganised demo, spinning about +Z so it reads as
  // a live render. Spin matches the per-frame clock used by MaybeInjectDemoShapes.
  const char *animEnv = std::getenv("GZ_O3DE_DEMO_ANIMATE");
  const bool animate = !(animEnv && animEnv[0] == '0');
  static const auto t0 = std::chrono::steady_clock::now();
  const double t = animate
      ? std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count()
      : 0.0;
  const double heroSpin = t * 0.7;

  O3deMeshData m;
  m.id = kDemoMeshId;
  m.pos[0] = 1.5; m.pos[1] = 0.0; m.pos[2] = 0.7;
  m.quat[0] = std::cos(heroSpin * 0.5); m.quat[1] = 0.0;
  m.quat[2] = 0.0;                      m.quat[3] = std::sin(heroSpin * 0.5);
  m.scale[0] = 1.1; m.scale[1] = 1.1; m.scale[2] = 1.1;
  // Distinctive cyan tint, sourced into a unique StandardPBR instance per mesh
  // (see SubmitMeshes). With the reorg's softened 300 cd point light the lit
  // faces no longer blow out to white, so the albedo is now visible; the tint
  // is also confirmed by the one-shot "[gz-o3de] M10 mesh ... set=1 compile=1"
  // telemetry.
  m.color[0] = 0.0f; m.color[1] = 0.85f; m.color[2] = 0.9f; m.color[3] = 1.0f;
  _meshes.push_back(m);
}

//////////////////////////////////////////////////
bool O3deBackend::RenderFrame(const O3deCameraData &_camera,
    const std::vector<O3deShapeData> &_shapes,
    const std::vector<O3deLightData> &_lights,
    const std::vector<O3deMeshData> &_meshes,
    uint32_t _width, uint32_t _height, uint8_t *_outRgba)
{
  Impl &d = *this->dataPtr;
  if (!d.ready || !_outRgba || _width == 0u || _height == 0u)
    return false;

  // Assemble the shape list on the caller's thread (only plain data crosses to
  // the render thread).
  std::vector<O3deShapeData> shapes = _shapes;
  MaybeInjectDemoShapes(shapes);
  std::vector<O3deLightData> lights = _lights;
  MaybeInjectDemoLights(lights);
  std::vector<O3deMeshData> meshes = _meshes;
  MaybeInjectDemoMeshData(meshes);

  // Post the input to the render thread and wait for a frame rendered after it.
  uint64_t startSeq = 0u;
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.pendingCamera = _camera;
    d.pendingShapes = std::move(shapes);
    d.pendingLights = std::move(lights);
    d.pendingMeshes = std::move(meshes);
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
    const std::vector<O3deLightData> &_lights,
    const std::vector<O3deMeshData> &_meshes,
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
  std::vector<O3deLightData> lights = _lights;
  MaybeInjectDemoLights(lights);
  std::vector<O3deMeshData> meshes = _meshes;
  MaybeInjectDemoMeshData(meshes);

  // Post the input to the render thread and wait for a frame rendered after it
  // (the frame is published into the shared image by RenderOneFrame()).
  uint64_t startSeq = 0u;
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.pendingCamera = _camera;
    d.pendingShapes = std::move(shapes);
    d.pendingLights = std::move(lights);
    d.pendingMeshes = std::move(meshes);
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
void O3deBackend::RegisterMesh(uint64_t _id, const gz::common::Mesh *_mesh)
{
  Impl &d = *this->dataPtr;
  // Extract on the caller's (gz) thread -- no Atom calls -- then hand the plain
  // geometry to the render thread, which builds the Atom model lazily. A null
  // or geometry-less mesh is treated as an unregister.
  MeshGeometryCpu geom = ExtractMeshGeometry(_mesh);
  std::lock_guard<std::mutex> lock(d.mutex);
  if (geom.Empty())
  {
    d.meshGeometry.erase(_id);
    d.meshUnregister.push_back(_id);
    return;
  }
  // Each mesh has a unique gz id and is registered once (CreateMeshImpl), so we
  // just stage the geometry; the render thread builds the model on first sight.
  // Re-registering the same id with new geometry is not a supported case here
  // (it would keep the already-built model); UnregisterMesh first if needed.
  d.meshGeometry[_id] = std::move(geom);
}

//////////////////////////////////////////////////
void O3deBackend::UnregisterMesh(uint64_t _id)
{
  Impl &d = *this->dataPtr;
  std::lock_guard<std::mutex> lock(d.mutex);
  d.meshGeometry.erase(_id);
  d.meshUnregister.push_back(_id);
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
        this->SubmitLights();
        this->SubmitMeshes();
        this->SubmitPrimitives();
        this->app->PumpSystemEventLoopUntilEmpty();
        this->app->TickSystem();
        this->app->Tick();
      }
      this->pipeline->RemoveFromRenderTick();
    }
    this->ApplyCamera(lw, lh);

    // DIAGNOSTIC (runtime-mesh-unlit): RenderDoc in-app capture trigger. Atom
    // renders OFFSCREEN to a windowless device (no swapchain), so RenderDoc's
    // F12/window capture only ever grabs the Qt swapchain frame (GUI chrome) and
    // never Atom's StandardPBR draws. The in-app API is the only way to capture a
    // presentation-less device. Gated by GZ_O3DE_RDC_FRAME=<frameNum>: when that
    // frame renders, bracket the Atom GPU submit so the .rdc contains the scene
    // draws (run the demo under `renderdoccmd capture` so the layer is loaded).
    static RENDERDOC_API_1_4_0 *s_rdoc = []() -> RENDERDOC_API_1_4_0 * {
      if (void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD))
      {
        if (auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
                dlsym(mod, "RENDERDOC_GetAPI")))
        {
          RENDERDOC_API_1_4_0 *api = nullptr;
          if (getApi(eRENDERDOC_API_Version_1_4_0,
                  reinterpret_cast<void **>(&api)) == 1)
            return api;
        }
      }
      return nullptr;
    }();
    const char *rdocFrameEnv = std::getenv("GZ_O3DE_RDC_FRAME");
    const bool rdocCapture = s_rdoc != nullptr && rdocFrameEnv != nullptr &&
        frameNum == std::atoi(rdocFrameEnv);
    // The RENDERDOC_DevicePointer must identify ATOM's device specifically -- with
    // two VkDevices in-process (Atom offscreen + Qt swapchain), NULL attaches to
    // the wrong (idle) one and captures zero draws. For Vulkan the device pointer
    // is the loader dispatch key: *(void**)VkDevice.
    void *rdocDevPtr = nullptr;
    if (rdocCapture)
    {
      if (AZ::RHI::Device *dev = AZ::RHI::RHISystemInterface::Get()->GetDevice(
              AZ::RHI::MultiDevice::DefaultDeviceIndex))
      {
        const VkDevice vkDev = AZ::Vulkan::GetDeviceNativeHandle(*dev);
        if (vkDev != VK_NULL_HANDLE)
          rdocDevPtr = *reinterpret_cast<void **>(vkDev);
      }
      std::fprintf(stderr,
          "[gz-o3de] RDOC StartFrameCapture frame=%d devPtr=%p\n",
          frameNum, rdocDevPtr);
      s_rdoc->StartFrameCapture(rdocDevPtr, nullptr);
    }

    // Render the scene into the exportable image. AuxGeom is re-submitted each
    // tick (the draw queue is consumed per rendered frame).
    this->pipeline->AddToRenderTick();
    for (int k = 0; k < 3; ++k)
    {
      this->SubmitLights();
      this->SubmitMeshes();
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

    if (rdocCapture)
    {
      const uint32_t rdocOk = s_rdoc->EndFrameCapture(rdocDevPtr, nullptr);
      std::fprintf(stderr, "[gz-o3de] RDOC EndFrameCapture ok=%u\n", rdocOk);
    }

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
      this->SubmitLights();
      this->SubmitMeshes();
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
    this->SubmitLights();
    this->SubmitMeshes();
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
