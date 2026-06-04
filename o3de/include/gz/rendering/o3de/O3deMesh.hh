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
#ifndef GZ_RENDERING_O3DE_O3DEMESH_HH_
#define GZ_RENDERING_O3DE_O3DEMESH_HH_

#include "gz/rendering/base/BaseMesh.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE mesh geometry.
    ///
    /// Started as the M5 Phase B no-op fallback (so callers funnelling
    /// through Scene::CreateMesh(...) -- e.g. BaseArrowVisual::Init's
    /// "rotation ring" -- could complete initialization). Since M9-B real
    /// meshes render through the MeshFeatureProcessor: CreateMeshImpl
    /// registers the gz-common geometry with the backend keyed by this
    /// mesh's id, and GatherFrame emits an O3deMeshData per frame (the
    /// geometry type stays GeometryType::OTHER, so it never produces
    /// AuxGeom draws). Since M12 the attached gz material's
    /// diffuse/metalness/roughness/texture ride the same snapshot.
    /// The submesh store stays EMPTY -- per-submesh materials are out of
    /// scope (see O3deSubMesh::SetMaterialImpl).
    class GZ_RENDERING_O3DE_VISIBLE O3deMesh :
      public BaseMesh<O3deGeometry>
    {
      /// \brief Constructor
      protected: O3deMesh();

      /// \brief Destructor
      public: virtual ~O3deMesh();

      /// \brief Get the (empty) submesh store backing this fallback mesh.
      protected: virtual SubMeshStorePtr SubMeshes() const override;

      /// \brief Get the mesh-level material (M12).
      ///
      /// BaseMesh::Material() reads the material from submesh 0 and returns
      /// null when the submesh store is empty -- which it always is here
      /// (the backend consumes the gz-common geometry directly; per-submesh
      /// materials are out of scope, see O3deSubMesh::SetMaterialImpl).
      /// Return the mesh-level material BaseMesh::SetMaterial stored
      /// instead, so O3deRenderTarget::GatherFrame sees the material the
      /// caller attached.
      public: virtual MaterialPtr Material() const override;

      /// \brief Empty store, allocated once in the constructor and shared.
      protected: O3deSubMeshStorePtr subMeshes;

      /// \brief Make the scene our friend so it can create meshes
      private: friend class O3deScene;
    };

    /// \brief O3DE no-op submesh fallback (M5 Phase B).
    ///
    /// Exists only to satisfy the BaseSubMeshStore<O3deSubMesh> template;
    /// instances are never produced by the M5 path (the store stays empty).
    class GZ_RENDERING_O3DE_VISIBLE O3deSubMesh :
      public BaseSubMesh<O3deObject>
    {
      /// \brief Constructor
      protected: O3deSubMesh();

      /// \brief Destructor
      public: virtual ~O3deSubMesh();

      /// \brief No-op: there is no underlying material on a M5 fallback mesh.
      public: virtual void SetMaterialImpl(MaterialPtr _material) override;

      /// \brief Make the scene our friend so it can create submeshes
      private: friend class O3deScene;
    };
    }
  }
}
#endif
