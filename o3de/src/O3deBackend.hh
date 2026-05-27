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
