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

#include "gz/rendering/o3de/O3deNode.hh"
#include "gz/rendering/o3de/O3deStorage.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deNode::O3deNode()
{
}

//////////////////////////////////////////////////
O3deNode::~O3deNode()
{
}

//////////////////////////////////////////////////
bool O3deNode::HasParent() const
{
  return this->parent != nullptr;
}

//////////////////////////////////////////////////
NodePtr O3deNode::Parent() const
{
  return this->parent;
}

//////////////////////////////////////////////////
math::Vector3d O3deNode::LocalScale() const
{
  return this->localScale;
}

//////////////////////////////////////////////////
bool O3deNode::InheritScale() const
{
  return this->inheritScale;
}

//////////////////////////////////////////////////
void O3deNode::SetInheritScale(bool _inherit)
{
  this->inheritScale = _inherit;
}

//////////////////////////////////////////////////
void O3deNode::SetLocalScaleImpl(const math::Vector3d &_scale)
{
  this->localScale = _scale;
}

//////////////////////////////////////////////////
NodeStorePtr O3deNode::Children() const
{
  return this->children;
}

//////////////////////////////////////////////////
bool O3deNode::AttachChild(NodePtr _child)
{
  O3deNodePtr derived = std::dynamic_pointer_cast<O3deNode>(_child);

  if (!derived)
  {
    gzerr << "Cannot attach node created by another render-engine"
          << std::endl;
    return false;
  }

  derived->SetParent(this->SharedThis());
  return true;
}

//////////////////////////////////////////////////
bool O3deNode::DetachChild(NodePtr _child)
{
  O3deNodePtr derived = std::dynamic_pointer_cast<O3deNode>(_child);

  if (!derived)
  {
    gzerr << "Cannot detach node created by another render-engine"
          << std::endl;
    return false;
  }

  derived->parent = nullptr;
  return true;
}

//////////////////////////////////////////////////
math::Pose3d O3deNode::RawLocalPose() const
{
  return this->pose;
}

//////////////////////////////////////////////////
void O3deNode::SetRawLocalPose(const math::Pose3d &_pose)
{
  this->pose = _pose;
}

//////////////////////////////////////////////////
void O3deNode::SetParent(O3deNodePtr _parent)
{
  this->parent = _parent;
}

//////////////////////////////////////////////////
void O3deNode::Init()
{
  this->children = O3deNodeStorePtr(new O3deNodeStore);
}

//////////////////////////////////////////////////
O3deNodePtr O3deNode::SharedThis()
{
  ObjectPtr object = shared_from_this();
  return std::dynamic_pointer_cast<O3deNode>(object);
}
