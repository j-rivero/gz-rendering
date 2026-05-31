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
#ifndef GZ_RENDERING_O3DE_O3DEARROWVISUAL_HH_
#define GZ_RENDERING_O3DE_O3DEARROWVISUAL_HH_

#include "gz/rendering/base/BaseArrowVisual.hh"
#include "gz/rendering/o3de/O3deVisual.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of an arrow visual.
    ///
    /// All behavior is inherited from BaseArrowVisual: Init() spawns the three
    /// child visuals (cone head + cylinder shaft + rotation-ring mesh) and the
    /// per-shape AuxGeom draws are issued by the gather/backend.
    class GZ_RENDERING_O3DE_VISIBLE O3deArrowVisual :
      public BaseArrowVisual<O3deVisual>
    {
      /// \brief Constructor
      protected: O3deArrowVisual();

      /// \brief Destructor
      public: virtual ~O3deArrowVisual();

      /// \brief Only the scene can instantiate an arrow visual
      private: friend class O3deScene;
    };
    }
  }
}
#endif
