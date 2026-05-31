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
    /// \brief O3DE no-op mesh fallback (M5 Phase B).
    ///
    /// Returns a real Mesh instance with an empty submesh store so callers
    /// that funnel through Scene::CreateMesh(...) -- in particular
    /// BaseArrowVisual::Init, which adds a "rotation ring" mesh geometry to
    /// its third child visual -- can complete initialization without crashing.
    ///
    /// The geometry type stays GeometryType::OTHER so ToBackendType() returns
    /// false in the per-frame gather; the mesh therefore contributes no
    /// AuxGeom draws. Real mesh rendering (asset import + MeshFeatureProcessor)
    /// is M9 in the post-beta1 roadmap.
    class GZ_RENDERING_O3DE_VISIBLE O3deMesh :
      public BaseMesh<O3deGeometry>
    {
      /// \brief Constructor
      protected: O3deMesh();

      /// \brief Destructor
      public: virtual ~O3deMesh();

      /// \brief Get the (empty) submesh store backing this fallback mesh.
      protected: virtual SubMeshStorePtr SubMeshes() const override;

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
