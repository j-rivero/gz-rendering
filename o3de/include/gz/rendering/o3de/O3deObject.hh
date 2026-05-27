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
#ifndef GZ_RENDERING_O3DE_O3DEOBJECT_HH_
#define GZ_RENDERING_O3DE_O3DEOBJECT_HH_

#include "gz/rendering/config.hh"
#include "gz/rendering/base/BaseObject.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"
#include "gz/rendering/o3de/Export.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Object class
    class GZ_RENDERING_O3DE_VISIBLE O3deObject :
      public BaseObject
    {
      /// \brief Constructor
      protected: O3deObject();

      /// \brief Destructor
      public: virtual ~O3deObject();

      // Documentation inherited
      public: virtual ScenePtr Scene() const override;

      /// \brief Pointer to the o3de scene
      protected: O3deScenePtr scene;

      /// \brief Make the o3de scene our friend so it can create objects
      private: friend class O3deScene;
    };
    }
  }
}
#endif
