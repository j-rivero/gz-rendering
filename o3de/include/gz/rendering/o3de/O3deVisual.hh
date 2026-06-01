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
#ifndef GZ_RENDERING_O3DE_O3DEVISUAL_HH_
#define GZ_RENDERING_O3DE_O3DEVISUAL_HH_

#include "gz/rendering/base/BaseVisual.hh"
#include "gz/rendering/o3de/O3deNode.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Visual class
    class GZ_RENDERING_O3DE_VISIBLE O3deVisual :
      public BaseVisual<O3deNode>
    {
      /// \brief Constructor
      protected: O3deVisual();

      /// \brief Destructor
      public: virtual ~O3deVisual();

      // Documentation inherited.
      protected: virtual GeometryStorePtr Geometries() const override;

      // Documentation inherited.
      protected: virtual bool AttachGeometry(GeometryPtr _geometry) override;

      // Documentation inherited.
      protected: virtual bool DetachGeometry(GeometryPtr _geometry) override;

      // Documentation inherited.
      protected: virtual void Init() override;

      // Documentation inherited.
      // Stores the flag (honoured by O3deRenderTarget::GatherFrame, which skips
      // hidden visuals) instead of emitting BaseVisual's "not supported" error.
      // gz-gui plugins such as InteractiveViewControl toggle the visibility of
      // their reference visuals every interaction, so the base error would spam
      // the console.
      public: virtual void SetVisible(bool _visible) override;

      /// \brief Whether this visual is currently visible. Visuals default to
      /// visible; GatherFrame uses this to decide whether to draw the visual.
      /// \return True if the visual should be drawn.
      public: bool Visible() const;

      /// \brief Get a shared pointer to this
      private: O3deVisualPtr SharedThis();

      /// \brief Pointer to the attached geometries
      protected: O3deGeometryStorePtr geometries;

      /// \brief Whether this visual is drawn (see SetVisible).
      protected: bool visible = true;

      /// \brief Make the scene our friend so it can create visuals
      private: friend class O3deScene;
    };
    }
  }
}
#endif
