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
#ifndef GZ_RENDERING_O3DE_O3DELIGHT_HH_
#define GZ_RENDERING_O3DE_O3DELIGHT_HH_

#include "gz/rendering/base/BaseLight.hh"
#include "gz/rendering/o3de/O3deNode.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Light class.
    ///
    /// M0 stub: light properties are stored but not yet wired into Atom.
    class GZ_RENDERING_O3DE_VISIBLE O3deLight :
      public BaseLight<O3deNode>
    {
      /// \brief Constructor
      protected: O3deLight();

      /// \brief Destructor
      public: virtual ~O3deLight();

      // Documentation inherited.
      public: virtual math::Color DiffuseColor() const override;

      // Documentation inherited.
      public: virtual void SetDiffuseColor(
                  const math::Color &_color) override;

      // Documentation inherited.
      public: virtual math::Color SpecularColor() const override;

      // Documentation inherited.
      public: virtual void SetSpecularColor(
                  const math::Color &_color) override;

      // Documentation inherited.
      public: virtual double AttenuationConstant() const override;

      // Documentation inherited.
      public: virtual void SetAttenuationConstant(double _value) override;

      // Documentation inherited.
      public: virtual double AttenuationLinear() const override;

      // Documentation inherited.
      public: virtual void SetAttenuationLinear(double _value) override;

      // Documentation inherited.
      public: virtual double AttenuationQuadratic() const override;

      // Documentation inherited.
      public: virtual void SetAttenuationQuadratic(double _value) override;

      // Documentation inherited.
      public: virtual double AttenuationRange() const override;

      // Documentation inherited.
      public: virtual void SetAttenuationRange(double _range) override;

      // Documentation inherited.
      public: virtual bool CastShadows() const override;

      // Documentation inherited.
      public: virtual void SetCastShadows(bool _castShadows) override;

      // Documentation inherited.
      public: virtual double Intensity() const override;

      // Documentation inherited.
      public: virtual void SetIntensity(double _intensity) override;

      /// \brief Diffuse color
      protected: math::Color diffuseColor;

      /// \brief Specular color
      protected: math::Color specularColor;

      /// \brief Attenuation constant factor
      protected: double attenConstant = 1.0;

      /// \brief Attenuation linear factor
      protected: double attenLinear = 0.0;

      /// \brief Attenuation quadratic factor
      protected: double attenQuadratic = 0.0;

      /// \brief Attenuation range
      protected: double attenRange = 100.0;

      /// \brief Whether the light casts shadows
      protected: bool castShadows = false;

      /// \brief Light intensity
      protected: double intensity = 1.0;

      /// \brief Make the scene our friend so it can create lights
      private: friend class O3deScene;
    };

    /// \brief O3DE implementation of the DirectionalLight class
    class GZ_RENDERING_O3DE_VISIBLE O3deDirectionalLight :
      public BaseDirectionalLight<O3deLight>
    {
      /// \brief Constructor
      protected: O3deDirectionalLight();

      /// \brief Destructor
      public: virtual ~O3deDirectionalLight();

      // Documentation inherited.
      public: virtual math::Vector3d Direction() const override;

      // Documentation inherited.
      public: virtual void SetDirection(const math::Vector3d &_dir) override;

      /// \brief Light direction
      protected: math::Vector3d direction;

      /// \brief Make the scene our friend so it can create lights
      private: friend class O3deScene;
    };

    /// \brief O3DE implementation of the PointLight class
    class GZ_RENDERING_O3DE_VISIBLE O3dePointLight :
      public BasePointLight<O3deLight>
    {
      /// \brief Constructor
      protected: O3dePointLight();

      /// \brief Destructor
      public: virtual ~O3dePointLight();

      /// \brief Make the scene our friend so it can create lights
      private: friend class O3deScene;
    };

    /// \brief O3DE implementation of the SpotLight class
    class GZ_RENDERING_O3DE_VISIBLE O3deSpotLight :
      public BaseSpotLight<O3deLight>
    {
      /// \brief Constructor
      protected: O3deSpotLight();

      /// \brief Destructor
      public: virtual ~O3deSpotLight();

      // Documentation inherited.
      public: virtual math::Vector3d Direction() const override;

      // Documentation inherited.
      public: virtual void SetDirection(const math::Vector3d &_dir) override;

      // Documentation inherited.
      public: virtual math::Angle InnerAngle() const override;

      // Documentation inherited.
      public: virtual void SetInnerAngle(const math::Angle &_angle) override;

      // Documentation inherited.
      public: virtual math::Angle OuterAngle() const override;

      // Documentation inherited.
      public: virtual void SetOuterAngle(const math::Angle &_angle) override;

      // Documentation inherited.
      public: virtual double Falloff() const override;

      // Documentation inherited.
      public: virtual void SetFalloff(double _falloff) override;

      /// \brief Light direction
      protected: math::Vector3d direction;

      /// \brief Inner angle of the spot light cone
      protected: math::Angle innerAngle;

      /// \brief Outer angle of the spot light cone
      protected: math::Angle outerAngle;

      /// \brief Falloff between inner and outer cone
      protected: double falloff = 1.0;

      /// \brief Make the scene our friend so it can create lights
      private: friend class O3deScene;
    };
    }
  }
}
#endif
