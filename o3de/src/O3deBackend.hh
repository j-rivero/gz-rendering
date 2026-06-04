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
#ifndef GZ_RENDERING_O3DE_O3DEBACKEND_HH_
#define GZ_RENDERING_O3DE_O3DEBACKEND_HH_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// NOTE: This header is the *only* bridge between the gz-rendering wrapper
// classes and the O3DE/Atom runtime. It is plain C++ on purpose: it exposes
// NO Atom or AzCore types. The implementation (O3deBackend.cc) is the single
// translation unit compiled with O3DE's compile model (-fno-exceptions,
// -fvisibility=hidden, C++20, O3DE defines) and linked against the prebuilt
// O3DE libraries; everything else in the component compiles with the normal
// gz-rendering flags and talks to Atom only through this interface.
namespace gz
{
  namespace common
  {
    class Mesh;
  }
  namespace rendering
  {
    /// \brief One primitive to draw this frame, in gz world coordinates.
    /// Plain data so the gz wrappers can populate it without any Atom type.
    struct O3deShapeData
    {
      /// \brief Primitive kind (mirrors O3deGeometry::GeometryType, plus
      /// FRUSTUM which is sourced from a FrustumVisual rather than a
      /// Geometry attached to a Visual).
      enum class Type : int
      {
        BOX = 0, SPHERE = 1, CYLINDER = 2, CONE = 3,
        PLANE = 4,    //!< Flat quad in the visual's local XY plane (scale = size).
        GRID = 5,     //!< Wireframe ground grid (see cellCount/cellLength).
        WIREBOX = 6,  //!< Wireframe box edges (see boxMin/boxMax, local AABB).
        CAPSULE = 7,  //!< Cylinder body + 2 hemisphere caps along local +Z (see
                      //!< capsuleRadius/capsuleLength).
        FRUSTUM = 8   //!< View frustum wireframe along local +X (see
                      //!< frustumNear/Far/HFov/AspectRatio).
      };

      Type type = Type::BOX;
      double pos[3] = {0.0, 0.0, 0.0};       //!< World position (gz frame).
      double quat[4] = {1.0, 0.0, 0.0, 0.0}; //!< World orientation w,x,y,z.
      double scale[3] = {1.0, 1.0, 1.0};     //!< World scale / dimensions.
      float color[4] = {0.8f, 0.8f, 0.8f, 1.0f}; //!< RGBA diffuse color.

      // Grid params (Type::GRID): a cellCount x cellCount grid of cellLength
      // squares centred on the origin in local XY; verticalCellCount > 0 adds
      // horizontal layers stacked along local +Z.
      int cellCount = 10;
      double cellLength = 1.0;
      int verticalCellCount = 0;

      // WireBox local axis-aligned box (Type::WIREBOX), before the world
      // pose/scale above is applied.
      double boxMin[3] = {-0.5, -0.5, -0.5};
      double boxMax[3] = {0.5, 0.5, 0.5};

      // Capsule params (Type::CAPSULE): cylinder body of length capsuleLength
      // along the visual's local +Z, with hemispherical caps of capsuleRadius
      // at each end. Total height = capsuleLength + 2 * capsuleRadius.
      double capsuleRadius = 0.5;
      double capsuleLength = 0.5;

      // Frustum params (Type::FRUSTUM): a perspective view frustum extending
      // along the visual's local +X (gz camera convention is +X forward, +Z
      // up). Near and far rectangles are sized from the horizontal FoV and
      // aspect ratio in the standard way; the backend draws the 12 edges
      // plus 4 apex-to-near-corner connectors as AuxGeom lines.
      double frustumNear = 0.1;
      double frustumFar = 1.0;
      double frustumHFov = 1.047;
      double frustumAspectRatio = 1.0;
    };

    /// \brief Everything an importing graphics context needs to alias the
    /// backend's exportable Vulkan colour image (M4 zero-copy interop). Consumed
    /// by both the GL importer (O3deGlInterop.cc) and the Vulkan->Vulkan importer
    /// (O3deVkInterop.cc). Plain data: the FDs are OS handles, no Vulkan/GL types
    /// leak through. The image is `R8G8B8A8_UNORM`, optimal tiling. \ref fd refers
    /// to the whole VMA memory block (\ref allocationSize); the image starts at
    /// \ref allocationOffset within it. The caller owns \ref fd / \ref semaphoreFd
    /// and must close (or transfer ownership of) them.
    struct O3deInteropImport
    {
      int fd = -1;                  //!< dup'd OPAQUE_FD for the backing memory.
      uint32_t width = 0u;          //!< Image width in pixels.
      uint32_t height = 0u;         //!< Image height in pixels.
      uint64_t allocationSize = 0u; //!< Size of the whole memory block the FD maps.
      uint64_t allocationOffset = 0u; //!< Byte offset of the image in that block.

      /// \brief dup'd OPAQUE_FD for the render-finished semaphore, or -1 if the
      /// producer does not export one (e.g. the static probe path needs no
      /// synchronisation). For the Stage B live render-into-image path this is a
      /// TIMELINE semaphore Atom signals when the per-frame copy into this image
      /// completes. The handle is stable across frames; a Vulkan importer imports
      /// it (VkImportSemaphoreFdInfoKHR) once and waits on \ref semaphoreWaitValue
      /// before sampling.
      int semaphoreFd = -1;

      /// \brief Timeline value the producer's GPU will signal \ref semaphoreFd to
      /// for the latest frame. The consumer waits for this value (it advances each
      /// frame). Meaningful only when \ref semaphoreFd >= 0.
      uint64_t semaphoreWaitValue = 0u;

      /// \brief Bumped by the producer whenever it (re)creates the exportable
      /// image (e.g. the initial probe -> the camera-sized live target, or a
      /// window resize). \ref fd and the geometry above refer to that
      /// generation's image; a consumer re-imports when this value changes.
      uint64_t generation = 0u;

      /// \brief True when the producer renders the live scene directly into this
      /// image every frame (Stage B). The image's contents AND its
      /// producer-side layout change per frame, so the consumer must re-acquire
      /// ownership + transition the layout each frame (from a colour-attachment
      /// layout). False for the static, uploaded-once probe (acquire once, from a
      /// shader-read layout).
      bool live = false;
    };

    /// \brief One scene light for this frame, in gz world coordinates.
    /// Plain data so the gz wrappers can populate it without any Atom type.
    /// Carries a stable id so the backend can round-trip Atom LightFeature-
    /// Processor handles across frames (acquire on first sighting, release
    /// when the id disappears).
    struct O3deLightData
    {
      /// \brief Light kind. Maps to the matching Atom feature processor.
      enum class Type : int
      {
        DIRECTIONAL = 0,  //!< Atom DirectionalLightFeatureProcessor (lux).
        POINT = 1,        //!< Atom SimplePointLightFeatureProcessor (candela).
        SPOT = 2          //!< Atom SimpleSpotLightFeatureProcessor (candela).
      };

      Type type = Type::DIRECTIONAL;
      uint32_t id = 0u;                      //!< Stable gz object id.
      double pos[3] = {0.0, 0.0, 0.0};       //!< World position (gz frame).
      double quat[4] = {1.0, 0.0, 0.0, 0.0}; //!< World orientation w,x,y,z.
      double dir[3] = {0.0, 0.0, -1.0};      //!< World direction (normalized),
                                             //!< used for DIRECTIONAL and SPOT.
      double diffuseColor[3] = {1.0, 1.0, 1.0};
      double intensity = 1.0;        //!< Photometric intensity scale.
      double attenRange = 100.0;     //!< Effective radius for POINT and SPOT.
      double innerAngle = 0.0;       //!< Spot inner cone angle (rad).
      double outerAngle = 0.5;       //!< Spot outer cone angle (rad).
    };

    /// \brief One mesh instance to draw this frame, in gz world coordinates
    /// (M9). Plain data: it carries only a stable id plus the world transform +
    /// colour, NOT the geometry. The geometry is registered once, out of band,
    /// via O3deBackend::RegisterMesh(id, ...); the backend builds the Atom model
    /// on its render thread on first sighting of the id and round-trips the
    /// MeshFeatureProcessor handle across frames (acquire on first sight, update
    /// the transform each frame, release when the id disappears) -- the same
    /// id-keyed lifecycle the lights use.
    struct O3deMeshData
    {
      uint64_t id = 0u;                      //!< Stable gz object id (== RegisterMesh id).
      double pos[3] = {0.0, 0.0, 0.0};       //!< World position (gz frame).
      double quat[4] = {1.0, 0.0, 0.0, 0.0}; //!< World orientation w,x,y,z.
      double scale[3] = {1.0, 1.0, 1.0};     //!< World scale.
      float color[4] = {0.8f, 0.8f, 0.8f, 1.0f}; //!< RGBA diffuse tint.
      // M11: StandardPBR factors applied to the per-mesh material instance.
      // metallic 0=dielectric, 1=metal (needs IBL to reflect convincingly);
      // roughness 0=mirror-sharp specular, 1=fully diffuse. Sentinel < 0 means
      // "leave the material default" so callers that don't care don't override.
      float metallic = -1.0f;   //!< StandardPBR metallic.factor (<0 = default).
      float roughness = -1.0f;  //!< StandardPBR roughness.factor (<0 = default).
      // M11 Phase B: when true the per-mesh material binds the procedural
      // checkerboard base-color texture (baseColor.textureMap + useTexture).
      bool textured = false;    //!< Bind the demo checker base-color texture.
      // M11 Phase D: a real base-color (albedo) texture FILE, decoded at runtime
      // via gz::common::Image and uploaded to Atom -- the path a real gz material
      // carries. Takes precedence over `textured` when non-empty.
      std::string texturePath;  //!< Albedo map file path (empty = none).
    };

    /// \brief Camera pose + projection for one frame, in gz world coordinates.
    struct O3deCameraData
    {
      double pos[3] = {0.0, 0.0, 0.0};       //!< World position (gz frame).
      double quat[4] = {1.0, 0.0, 0.0, 0.0}; //!< World orientation w,x,y,z.
      double hfov = 1.047;                   //!< Horizontal field of view (rad).
      double nearClip = 0.1;                 //!< Near clip distance.
      double farClip = 1000.0;               //!< Far clip distance.
    };

    /// \brief Owns the embedded O3DE application runtime and the offscreen
    /// Atom render pipeline. A process-wide singleton: the O3DE runtime is
    /// brought up once and never torn down (tearing down the RPISystem while
    /// scene/buffer instances are live corrupts the Vulkan RHI on shutdown,
    /// and gz render-engine plugins are dlopen'd RTLD_NODELETE regardless).
    class O3deBackend
    {
      /// \brief Get the process-wide backend instance.
      public: static O3deBackend &Instance();

      /// \brief Bring up the O3DE runtime: host an AzGameFramework
      /// GameApplication, load the Atom gems, settle the RHI device and
      /// create the offscreen render-to-texture pipeline. Idempotent;
      /// safe to call more than once.
      /// \return True once the runtime is up and the pipeline is ready.
      public: bool Bootstrap();

      /// \brief True if Bootstrap() has succeeded and rendering is possible.
      public: bool IsReady() const;

      /// \brief Stop and join the dedicated render thread. Removes the only
      /// teardown race we own: a foreign thread ticking the runtime while the
      /// process tears down (the original exit-time SIGABRT). Idempotent.
      ///
      /// It deliberately does NOT tear down the O3DE/Vulkan runtime: both
      /// tearing it down and leaking it crash in upstream GPU driver/engine
      /// teardown at process exit. Bootstrap() registers an std::atexit handler
      /// that calls this and then std::quick_exit()s to skip those crashing
      /// destructors entirely (see the long note at the Bootstrap() call site).
      public: void Shutdown();

      /// \brief Render one offscreen frame of the given camera + primitives
      /// and read it back into \p _outRgba as tightly packed RGBA8888.
      ///
      /// The primitives are drawn with AuxGeom from the supplied scene
      /// contents. The call is synchronous: it advances the O3DE tick loop
      /// until the frame-capture readback completes, then copies the pixels out.
      /// \param[in] _camera Camera pose + projection (gz world frame).
      /// \param[in] _shapes Primitives to draw (gz world frame).
      /// \param[in] _width  Target width in pixels.
      /// \param[in] _height Target height in pixels.
      /// \param[out] _outRgba Caller-owned buffer of at least
      ///   _width * _height * 4 bytes.
      /// \return True if a frame was captured and copied.
      public: bool RenderFrame(const O3deCameraData &_camera,
                  const std::vector<O3deShapeData> &_shapes,
                  const std::vector<O3deLightData> &_lights,
                  const std::vector<O3deMeshData> &_meshes,
                  uint32_t _width, uint32_t _height, uint8_t *_outRgba);

      /// \brief Drive one offscreen frame of the given camera + primitives and
      /// publish it into the exportable interop colour image (M4 native
      /// Vulkan->Vulkan path), recreating that image at \p _width x \p _height
      /// if needed. Unlike RenderFrame() the pixels are NOT read back to the
      /// CPU; the rendered frame lands in the shared image that
      /// GetInteropImport() exposes, for a consumer to sample zero-copy.
      /// No-op (returns false) unless the plugin was built with
      /// -DGZ_O3DE_INTEROP=ON and the runtime was started with GZ_O3DE_INTEROP.
      /// Call once per displayed frame from the consumer's render path.
      /// \return True if a frame was rendered into the shared image.
      public: bool RenderFrameForInterop(const O3deCameraData &_camera,
                  const std::vector<O3deShapeData> &_shapes,
                  const std::vector<O3deLightData> &_lights,
                  const std::vector<O3deMeshData> &_meshes,
                  uint32_t _width, uint32_t _height);

      /// \brief Register (or replace) the geometry for a mesh id (M9). Called
      /// once per mesh from the gz thread (O3deScene::CreateMeshImpl). Only the
      /// plain vertex/index data is extracted + cached here; the Atom model is
      /// built lazily on the render thread the first time a matching
      /// O3deMeshData id is rendered (Atom asset construction must stay on the
      /// render thread). Safe to call while rendering. A null or empty mesh
      /// unregisters the id.
      /// \param[in] _id    Stable gz object id, matched against O3deMeshData::id.
      /// \param[in] _mesh  Source geometry (its triangle submeshes are merged).
      public: void RegisterMesh(uint64_t _id, const gz::common::Mesh *_mesh);

      /// \brief Drop a mesh id's cached geometry/model and release its Atom
      /// handle on the next render tick (M9). No-op for an unknown id.
      public: void UnregisterMesh(uint64_t _id);

      /// \brief Get import handles for the exportable interop colour image
      /// (M4 zero-copy path). Only valid when the plugin was built with
      /// -DGZ_O3DE_INTEROP=ON and the runtime was started with GZ_O3DE_INTEROP
      /// set; otherwise returns false. On success \p _out.fd is a fresh dup the
      /// caller must close. The image is filled with a deterministic gradient
      /// (R=x, G=y, B=128) so a GL importer can verify a correct round-trip.
      /// Safe to call from another thread (it dups a stored FD; no O3DE work).
      /// \param[out] _out Import handles + geometry.
      /// \return True if interop is built+enabled and the FD was produced.
      public: bool GetInteropImport(O3deInteropImport &_out);

      /// \brief Constructor. Use Instance().
      private: O3deBackend();

      /// \brief Destructor. Never frees the O3DE runtime (see class note).
      private: ~O3deBackend();

      public: O3deBackend(const O3deBackend &) = delete;
      public: O3deBackend &operator=(const O3deBackend &) = delete;

      /// \brief Private Atom-owning implementation (hidden from gz headers).
      private: class Impl;

      /// \brief Pointer to private data.
      private: Impl *dataPtr;
    };
  }
}
#endif
