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
#ifndef GZ_RENDERING_O3DE_O3DERENDERENGINE_HH_
#define GZ_RENDERING_O3DE_O3DERENDERENGINE_HH_

#include <map>
#include <string>

#include <gz/common/SingletonT.hh>

#include "gz/rendering/GraphicsAPI.hh"
#include "gz/rendering/RenderEnginePlugin.hh"
#include "gz/rendering/base/BaseRenderEngine.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"
#include "gz/rendering/o3de/Export.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief Plugin for loading the O3DE render engine
    class GZ_RENDERING_O3DE_VISIBLE O3deRenderEnginePlugin :
      public RenderEnginePlugin
    {
      /// \brief Constructor
      public: O3deRenderEnginePlugin() = default;

      /// \brief Destructor
      public: virtual ~O3deRenderEnginePlugin() = default;

      /// \brief Get the name of the render engine loaded by this plugin.
      /// \return Name of render engine
      public: virtual std::string Name() const override;

      /// \brief Get a pointer to the render engine loaded by this plugin.
      /// \return Render engine instance
      public: virtual RenderEngine *Engine() const override;
    };

    /// \brief O3DE/Atom implementation of the render engine.
    ///
    /// M0 stub: this brings up no Atom subsystems yet; it only manages
    /// the scene store so the gz-gui plumbing can be exercised. It reports
    /// GraphicsAPI()==VULKAN so gz-gui's MinimalScene uses its built-in
    /// CPU-readback path (camera->Copy()).
    class GZ_RENDERING_O3DE_VISIBLE O3deRenderEngine :
      public virtual BaseRenderEngine
    {
      /// \brief Constructor
      private: O3deRenderEngine();

      /// \brief Destructor
      public: virtual ~O3deRenderEngine();

      // Documentation inherited.
      public: virtual bool IsEnabled() const override;

      // Documentation inherited.
      public: virtual std::string Name() const override;

      // Documentation inherited.
      public: virtual rendering::GraphicsAPI GraphicsAPI() const override;

      // Documentation inherited.
      protected: virtual bool LoadImpl(
          const std::map<std::string, std::string> &_params) override;

      // Documentation inherited.
      protected: virtual bool InitImpl() override;

      // Documentation inherited.
      protected: virtual ScenePtr CreateSceneImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual SceneStorePtr Scenes() const override;

      /// \brief Get a pointer to the render engine
      /// \return Pointer to the render engine
      public: static O3deRenderEngine *Instance();

      /// \brief A list of scenes managed by the engine
      private: O3deSceneStorePtr scenes;

      /// \brief Singleton setup
      private: friend class gz::common::SingletonT<O3deRenderEngine>;
    };
    }
  }
}
#endif
