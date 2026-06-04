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
#include "gz/rendering/o3de/O3deMesh.hh"
#include "gz/rendering/o3de/O3deStorage.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deMesh::O3deMesh()
{
  // Pre-allocate an empty submesh store; SubMeshes() hands it back. Allocating
  // here (and not lazily) keeps SubMeshes() a const-qualified pure-getter.
  this->subMeshes = O3deSubMeshStorePtr(new O3deSubMeshStore);
}

//////////////////////////////////////////////////
O3deMesh::~O3deMesh() = default;

//////////////////////////////////////////////////
SubMeshStorePtr O3deMesh::SubMeshes() const
{
  return this->subMeshes;
}

//////////////////////////////////////////////////
MaterialPtr O3deMesh::Material() const
{
  // Not BaseMesh::Material(): that reads submesh 0 and our store is empty,
  // so it would always return null (see the header comment).
  return this->material;
}

//////////////////////////////////////////////////
O3deSubMesh::O3deSubMesh() = default;

//////////////////////////////////////////////////
O3deSubMesh::~O3deSubMesh() = default;

//////////////////////////////////////////////////
void O3deSubMesh::SetMaterialImpl(MaterialPtr /*_material*/)
{
  // No-op: there is no underlying mesh resource on the M5 fallback path.
  // Real material assignment lands with the M9 mesh asset import.
}
