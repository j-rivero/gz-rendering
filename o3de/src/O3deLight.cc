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
#include "gz/rendering/o3de/O3deLight.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deLight::O3deLight()
{
}

//////////////////////////////////////////////////
O3deLight::~O3deLight()
{
}

//////////////////////////////////////////////////
math::Color O3deLight::DiffuseColor() const
{
  return this->diffuseColor;
}

//////////////////////////////////////////////////
void O3deLight::SetDiffuseColor(const math::Color &_color)
{
  this->diffuseColor = _color;
}

//////////////////////////////////////////////////
math::Color O3deLight::SpecularColor() const
{
  return this->specularColor;
}

//////////////////////////////////////////////////
void O3deLight::SetSpecularColor(const math::Color &_color)
{
  this->specularColor = _color;
}

//////////////////////////////////////////////////
double O3deLight::AttenuationConstant() const
{
  return this->attenConstant;
}

//////////////////////////////////////////////////
void O3deLight::SetAttenuationConstant(double _value)
{
  this->attenConstant = _value;
}

//////////////////////////////////////////////////
double O3deLight::AttenuationLinear() const
{
  return this->attenLinear;
}

//////////////////////////////////////////////////
void O3deLight::SetAttenuationLinear(double _value)
{
  this->attenLinear = _value;
}

//////////////////////////////////////////////////
double O3deLight::AttenuationQuadratic() const
{
  return this->attenQuadratic;
}

//////////////////////////////////////////////////
void O3deLight::SetAttenuationQuadratic(double _value)
{
  this->attenQuadratic = _value;
}

//////////////////////////////////////////////////
double O3deLight::AttenuationRange() const
{
  return this->attenRange;
}

//////////////////////////////////////////////////
void O3deLight::SetAttenuationRange(double _range)
{
  this->attenRange = _range;
}

//////////////////////////////////////////////////
bool O3deLight::CastShadows() const
{
  return this->castShadows;
}

//////////////////////////////////////////////////
void O3deLight::SetCastShadows(bool _castShadows)
{
  this->castShadows = _castShadows;
}

//////////////////////////////////////////////////
double O3deLight::Intensity() const
{
  return this->intensity;
}

//////////////////////////////////////////////////
void O3deLight::SetIntensity(double _intensity)
{
  this->intensity = _intensity;
}

//////////////////////////////////////////////////
O3deDirectionalLight::O3deDirectionalLight()
{
}

//////////////////////////////////////////////////
O3deDirectionalLight::~O3deDirectionalLight()
{
}

//////////////////////////////////////////////////
math::Vector3d O3deDirectionalLight::Direction() const
{
  return this->direction;
}

//////////////////////////////////////////////////
void O3deDirectionalLight::SetDirection(const math::Vector3d &_dir)
{
  this->direction = _dir;
}

//////////////////////////////////////////////////
O3dePointLight::O3dePointLight()
{
}

//////////////////////////////////////////////////
O3dePointLight::~O3dePointLight()
{
}

//////////////////////////////////////////////////
O3deSpotLight::O3deSpotLight()
{
}

//////////////////////////////////////////////////
O3deSpotLight::~O3deSpotLight()
{
}

//////////////////////////////////////////////////
math::Vector3d O3deSpotLight::Direction() const
{
  return this->direction;
}

//////////////////////////////////////////////////
void O3deSpotLight::SetDirection(const math::Vector3d &_dir)
{
  this->direction = _dir;
}

//////////////////////////////////////////////////
math::Angle O3deSpotLight::InnerAngle() const
{
  return this->innerAngle;
}

//////////////////////////////////////////////////
void O3deSpotLight::SetInnerAngle(const math::Angle &_angle)
{
  this->innerAngle = _angle;
}

//////////////////////////////////////////////////
math::Angle O3deSpotLight::OuterAngle() const
{
  return this->outerAngle;
}

//////////////////////////////////////////////////
void O3deSpotLight::SetOuterAngle(const math::Angle &_angle)
{
  this->outerAngle = _angle;
}

//////////////////////////////////////////////////
double O3deSpotLight::Falloff() const
{
  return this->falloff;
}

//////////////////////////////////////////////////
void O3deSpotLight::SetFalloff(double _falloff)
{
  this->falloff = _falloff;
}
