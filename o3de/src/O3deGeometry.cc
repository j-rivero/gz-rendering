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
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deMaterial.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deGeometry::O3deGeometry()
{
}

//////////////////////////////////////////////////
O3deGeometry::~O3deGeometry()
{
}

//////////////////////////////////////////////////
O3deGeometry::GeometryType O3deGeometry::Type() const
{
  return this->geometryType;
}

//////////////////////////////////////////////////
void O3deGeometry::SetType(GeometryType _type)
{
  this->geometryType = _type;
}

//////////////////////////////////////////////////
bool O3deGeometry::HasParent() const
{
  return this->parent != nullptr;
}

//////////////////////////////////////////////////
VisualPtr O3deGeometry::Parent() const
{
  return this->parent;
}

//////////////////////////////////////////////////
MaterialPtr O3deGeometry::Material() const
{
  return this->material;
}

//////////////////////////////////////////////////
void O3deGeometry::SetMaterial(MaterialPtr _material, bool /*_unique*/)
{
  this->material = std::dynamic_pointer_cast<O3deMaterial>(_material);
}

//////////////////////////////////////////////////
void O3deGeometry::SetParent(O3deVisualPtr _parent)
{
  this->parent = _parent;
}
