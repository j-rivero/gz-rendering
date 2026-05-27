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
#ifndef GZ_RENDERING_O3DE_O3DENODE_HH_
#define GZ_RENDERING_O3DE_O3DENODE_HH_

#include "gz/rendering/base/BaseNode.hh"
#include "gz/rendering/o3de/O3deObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Node class.
    ///
    /// For the M0 stub this stores the transform hierarchy entirely in
    /// member variables; no O3DE/Atom transform object is wrapped yet.
    class GZ_RENDERING_O3DE_VISIBLE O3deNode :
      public BaseNode<O3deObject>
    {
      /// \brief Constructor
      protected: O3deNode();

      /// \brief Destructor
      public: virtual ~O3deNode();

      // Documentation inherited.
      public: virtual bool HasParent() const override;

      // Documentation inherited.
      public: virtual NodePtr Parent() const override;

      // Documentation inherited.
      public: virtual math::Vector3d LocalScale() const override;

      // Documentation inherited.
      public: virtual bool InheritScale() const override;

      // Documentation inherited.
      public: virtual void SetInheritScale(bool _inherit) override;

      // Documentation inherited.
      protected: virtual void SetLocalScaleImpl(
                     const math::Vector3d &_scale) override;

      // Documentation inherited.
      protected: virtual NodeStorePtr Children() const override;

      // Documentation inherited.
      protected: virtual bool AttachChild(NodePtr _child) override;

      // Documentation inherited.
      protected: virtual bool DetachChild(NodePtr _child) override;

      // Documentation inherited.
      protected: virtual math::Pose3d RawLocalPose() const override;

      // Documentation inherited.
      protected: virtual void SetRawLocalPose(const math::Pose3d &_pose)
                     override;

      /// \brief Set the parent node
      /// \param[in] _parent The parent node
      protected: virtual void SetParent(O3deNodePtr _parent);

      // Documentation inherited.
      protected: virtual void Init() override;

      /// \brief Get a shared pointer to this
      private: O3deNodePtr SharedThis();

      /// \brief Pointer to the parent node
      protected: O3deNodePtr parent;

      /// \brief Raw local pose (relative to parent)
      protected: math::Pose3d pose;

      /// \brief Local scale
      protected: math::Vector3d localScale = math::Vector3d::One;

      /// \brief Whether this node inherits scale from its parent
      protected: bool inheritScale = true;

      /// \brief A list of child nodes
      protected: O3deNodeStorePtr children;

      /// \brief Make the visual our friend so it can set the parent
      private: friend class O3deVisual;
    };
    }
  }
}
#endif
