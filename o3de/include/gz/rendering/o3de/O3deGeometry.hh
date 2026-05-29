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
#ifndef GZ_RENDERING_O3DE_O3DEGEOMETRY_HH_
#define GZ_RENDERING_O3DE_O3DEGEOMETRY_HH_

#include "gz/rendering/base/BaseGeometry.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Geometry class.
    ///
    /// For the M0 stub, every primitive (box, sphere, cylinder, ...) is
    /// represented by this single concrete type. It records its parent
    /// visual and material so a later milestone can translate it into an
    /// Atom AuxGeom draw call; it renders nothing on its own yet.
    class GZ_RENDERING_O3DE_VISIBLE O3deGeometry :
      public BaseGeometry<O3deObject>
    {
      /// \brief The kind of primitive this geometry represents. Drives which
      /// AuxGeom draw call the render backend issues for it.
      public: enum class GeometryType
      {
        OTHER = 0,
        BOX,
        SPHERE,
        CYLINDER,
        CONE,
        PLANE,
        GRID,
        WIREBOX
      };

      /// \brief Constructor
      protected: O3deGeometry();

      /// \brief Destructor
      public: virtual ~O3deGeometry();

      /// \brief Get the primitive type of this geometry.
      /// \return The primitive type
      public: GeometryType Type() const;

      /// \brief Set the primitive type of this geometry.
      /// \param[in] _type The primitive type
      public: void SetType(GeometryType _type);

      // Documentation inherited.
      public: virtual bool HasParent() const override;

      // Documentation inherited.
      public: virtual VisualPtr Parent() const override;

      // Documentation inherited.
      public: virtual MaterialPtr Material() const override;

      // Documentation inherited.
      public: virtual void SetMaterial(MaterialPtr _material,
                  bool _unique = true) override;

      /// \brief Set the parent visual of this geometry
      /// \param[in] _parent Parent visual
      protected: virtual void SetParent(O3deVisualPtr _parent);

      /// \brief Parent visual
      protected: O3deVisualPtr parent;

      /// \brief Material assigned to this geometry
      protected: O3deMaterialPtr material;

      /// \brief Primitive type recorded by the scene factory.
      protected: GeometryType geometryType = GeometryType::OTHER;

      /// \brief Make the visual our friend so it can set the parent
      private: friend class O3deVisual;

      /// \brief Make the scene our friend so it can create geometries
      private: friend class O3deScene;
    };
    }
  }
}
#endif
