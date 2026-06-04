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
#ifndef GZ_RENDERING_O3DE_O3DEMATERIAL_HH_
#define GZ_RENDERING_O3DE_O3DEMATERIAL_HH_

#include <memory>
#include <string>

#include "gz/rendering/base/BaseMaterial.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Material class.
    ///
    /// Colors and common flags are held by BaseMaterial; the PBR
    /// texture/roughness/metalness are stored here (BaseMaterial's
    /// accessors for those are no-ops). No Atom-side object exists per
    /// gz material. Properties cross to the renderer via the
    /// per-frame snapshot: O3deRenderTarget::GatherFrame reads Diffuse /
    /// Metalness / Roughness / Texture into O3deMeshData (M12) and the
    /// backend applies them to the per-mesh StandardPBR material instance
    /// (M11). Mesh geometries only; AuxGeom primitives use the flat
    /// diffuse colour.
    class GZ_RENDERING_O3DE_VISIBLE O3deMaterial :
      public BaseMaterial<O3deObject>
    {
      /// \brief Constructor
      protected: O3deMaterial();

      /// \brief Destructor
      public: virtual ~O3deMaterial();

      // BaseMaterial's implementations of the PBR texture/metalness/
      // roughness accessors are no-ops (it stores only the color and
      // common flags), so this class stores them itself.
      // O3deRenderTarget::GatherFrame reads them into the per-frame
      // snapshot (M12). Defaults: dielectric (metalness 0) and fully
      // diffuse (roughness 1) so a color-only material renders plain
      // diffuse rather than a mirror.

      // Documentation inherited.
      public: virtual void SetTexture(const std::string &_texture,
          const std::shared_ptr<const common::Image> &_img) override;

      // Documentation inherited.
      public: virtual std::string Texture() const override;

      // Documentation inherited.
      public: virtual bool HasTexture() const override;

      // Documentation inherited.
      public: virtual void ClearTexture() override;

      // Documentation inherited.
      public: virtual void SetRoughness(const float _roughness) override;

      // Documentation inherited.
      public: virtual float Roughness() const override;

      // Documentation inherited.
      public: virtual void SetMetalness(const float _metalness) override;

      // Documentation inherited.
      public: virtual float Metalness() const override;

      /// \brief Albedo (base color) texture file path; empty = none.
      private: std::string texture;

      /// \brief StandardPBR roughness factor (1 = fully diffuse).
      /// Deliberately 1.0 (not gz-common's Pbr default 0.5): a color-only
      /// material should read as plain Lambert, with no specular sheen.
      private: float roughness = 1.0f;

      /// \brief StandardPBR metalness factor (0 = dielectric).
      private: float metalness = 0.0f;

      /// \brief Make the scene our friend so it can create materials
      private: friend class O3deScene;
    };
    }
  }
}
#endif
