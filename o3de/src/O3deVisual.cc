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
#include <gz/common/Console.hh>

#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deVisual.hh"
#include "gz/rendering/o3de/O3deStorage.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deVisual::O3deVisual()
{
}

//////////////////////////////////////////////////
O3deVisual::~O3deVisual()
{
}

//////////////////////////////////////////////////
GeometryStorePtr O3deVisual::Geometries() const
{
  return this->geometries;
}

//////////////////////////////////////////////////
bool O3deVisual::AttachGeometry(GeometryPtr _geometry)
{
  O3deGeometryPtr derived =
      std::dynamic_pointer_cast<O3deGeometry>(_geometry);

  if (!derived)
  {
    gzerr << "Cannot attach geometry created by another render-engine"
          << std::endl;
    return false;
  }

  derived->SetParent(this->SharedThis());
  return true;
}

//////////////////////////////////////////////////
bool O3deVisual::DetachGeometry(GeometryPtr _geometry)
{
  O3deGeometryPtr derived =
      std::dynamic_pointer_cast<O3deGeometry>(_geometry);

  if (!derived)
  {
    gzerr << "Cannot detach geometry created by another render-engine"
          << std::endl;
    return false;
  }

  derived->SetParent(nullptr);
  return true;
}

//////////////////////////////////////////////////
void O3deVisual::Init()
{
  BaseVisual::Init();
  this->geometries = O3deGeometryStorePtr(new O3deGeometryStore);
}

//////////////////////////////////////////////////
O3deVisualPtr O3deVisual::SharedThis()
{
  ObjectPtr object = shared_from_this();
  return std::dynamic_pointer_cast<O3deVisual>(object);
}
