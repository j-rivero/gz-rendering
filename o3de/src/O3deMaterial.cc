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
#include "gz/rendering/o3de/O3deMaterial.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deMaterial::O3deMaterial()
{
}

//////////////////////////////////////////////////
O3deMaterial::~O3deMaterial()
{
}

//////////////////////////////////////////////////
void O3deMaterial::SetTexture(const std::string &_texture,
    const std::shared_ptr<const common::Image> &/*_img*/)
{
  // The backend decodes from the file path (FileBaseColorImage, M11-D);
  // a caller-supplied in-memory image is not consumed yet.
  this->texture = _texture;
}

//////////////////////////////////////////////////
std::string O3deMaterial::Texture() const
{
  return this->texture;
}

//////////////////////////////////////////////////
bool O3deMaterial::HasTexture() const
{
  return !this->texture.empty();
}

//////////////////////////////////////////////////
void O3deMaterial::ClearTexture()
{
  this->texture.clear();
}

//////////////////////////////////////////////////
void O3deMaterial::SetRoughness(const float _roughness)
{
  this->roughness = _roughness;
}

//////////////////////////////////////////////////
float O3deMaterial::Roughness() const
{
  return this->roughness;
}

//////////////////////////////////////////////////
void O3deMaterial::SetMetalness(const float _metalness)
{
  this->metalness = _metalness;
}

//////////////////////////////////////////////////
float O3deMaterial::Metalness() const
{
  return this->metalness;
}
