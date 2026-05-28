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
  namespace rendering
  {
    /// \brief One primitive to draw this frame, in gz world coordinates.
    /// Plain data so the gz wrappers can populate it without any Atom type.
    struct O3deShapeData
    {
      /// \brief Primitive kind (mirrors O3deGeometry::GeometryType).
      enum class Type : int { BOX = 0, SPHERE = 1, CYLINDER = 2, CONE = 3 };

      Type type = Type::BOX;
      double pos[3] = {0.0, 0.0, 0.0};       //!< World position (gz frame).
      double quat[4] = {1.0, 0.0, 0.0, 0.0}; //!< World orientation w,x,y,z.
      double scale[3] = {1.0, 1.0, 1.0};     //!< World scale / dimensions.
      float color[4] = {0.8f, 0.8f, 0.8f, 1.0f}; //!< RGBA diffuse color.
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
      /// producer does not export one yet. A static, uploaded-once image needs
      /// no synchronisation (the GL self-test confirmed this); a live
      /// render-into-shared-image target does -- exporting this semaphore is the
      /// M4 producer-side "semaphore sync" work that remains. A Vulkan importer
      /// waits on it (VkImportSemaphoreFdInfoKHR) before sampling.
      int semaphoreFd = -1;
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
                  uint32_t _width, uint32_t _height, uint8_t *_outRgba);

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
