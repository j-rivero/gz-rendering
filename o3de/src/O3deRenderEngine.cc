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
#include <cstdint>

#include <gz/common/Console.hh>
#include <gz/common/SingletonT.hh>
#include <gz/plugin/Register.hh>

#include "gz/rendering/o3de/O3deRenderEngine.hh"
#include "gz/rendering/o3de/O3deScene.hh"
#include "gz/rendering/o3de/O3deStorage.hh"

#include "O3deBackend.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
// O3deRenderEnginePlugin
//////////////////////////////////////////////////
std::string O3deRenderEnginePlugin::Name() const
{
  return O3deRenderEngine::Instance()->Name();
}

//////////////////////////////////////////////////
RenderEngine *O3deRenderEnginePlugin::Engine() const
{
  return O3deRenderEngine::Instance();
}

//////////////////////////////////////////////////
// O3deRenderEngine
//////////////////////////////////////////////////
O3deRenderEngine::O3deRenderEngine()
{
}

//////////////////////////////////////////////////
O3deRenderEngine::~O3deRenderEngine()
{
}

//////////////////////////////////////////////////
bool O3deRenderEngine::IsEnabled() const
{
  return true;
}

//////////////////////////////////////////////////
std::string O3deRenderEngine::Name() const
{
  return "o3de";
}

//////////////////////////////////////////////////
GraphicsAPI O3deRenderEngine::GraphicsAPI() const
{
  static int n = 0;
  if (n++ < 4)
    gzmsg << "[gz-o3de] GraphicsAPI() -> VULKAN (call " << n << ")" << std::endl;
  // O3DE/Atom renders through Vulkan on Linux. Reporting VULKAN makes
  // gz-gui's MinimalScene use its built-in CPU-readback path (camera->Copy)
  // rather than expecting a shareable GL texture.
  return GraphicsAPI::VULKAN;
}

//////////////////////////////////////////////////
bool O3deRenderEngine::LoadImpl(
    const std::map<std::string, std::string> &_params)
{
  // When gz-gui runs its Vulkan RHI backend it injects Qt's raw Vulkan handles
  // as decimal uintptr_t strings. We keep them as opaque pointers; O3deCamera
  // casts them to import Atom's exported colour image onto Qt's device for
  // native Vulkan->Vulkan display. Absent these (the OpenGL backend), the engine
  // falls back to the CPU-readback Copy() path.
  auto toHandle = [&_params](const char *_key) -> void *
  {
    auto it = _params.find(_key);
    if (it == _params.end())
      return nullptr;
    return reinterpret_cast<void *>(
        static_cast<uintptr_t>(std::stoull(it->second)));
  };
  if (_params.count("vulkan"))
  {
    this->qtVkInstance = toHandle("vulkan_instance");
    this->qtVkPhysicalDevice = toHandle("vulkan_physical_device");
    this->qtVkDevice = toHandle("vulkan_device");
    this->qtVkGraphicsQueue = toHandle("vulkan_graphics_queue");
    gzmsg << "[gz-o3de] Vulkan backend: Qt device=" << this->qtVkDevice
          << " physicalDevice=" << this->qtVkPhysicalDevice
          << " queue=" << this->qtVkGraphicsQueue << std::endl;
  }

  // Bring up the embedded O3DE runtime once. This hosts an AzGameFramework
  // GameApplication, loads the Atom gems and creates the offscreen render
  // pipeline. It is never torn down for the life of the process.
  if (!O3deBackend::Instance().Bootstrap())
  {
    gzerr << "Failed to bootstrap the embedded O3DE/Atom runtime" << std::endl;
    return false;
  }
  return true;
}

//////////////////////////////////////////////////
void *O3deRenderEngine::QtVulkanInstance() const
{
  return this->qtVkInstance;
}

//////////////////////////////////////////////////
void *O3deRenderEngine::QtVulkanPhysicalDevice() const
{
  return this->qtVkPhysicalDevice;
}

//////////////////////////////////////////////////
void *O3deRenderEngine::QtVulkanDevice() const
{
  return this->qtVkDevice;
}

//////////////////////////////////////////////////
void *O3deRenderEngine::QtVulkanGraphicsQueue() const
{
  return this->qtVkGraphicsQueue;
}

//////////////////////////////////////////////////
bool O3deRenderEngine::InitImpl()
{
  this->scenes = O3deSceneStorePtr(new O3deSceneStore);
  return true;
}

//////////////////////////////////////////////////
ScenePtr O3deRenderEngine::CreateSceneImpl(unsigned int _id,
    const std::string &_name)
{
  O3deScenePtr scene = O3deScenePtr(new O3deScene(_id, _name));
  this->scenes->Add(scene);
  return scene;
}

//////////////////////////////////////////////////
SceneStorePtr O3deRenderEngine::Scenes() const
{
  return this->scenes;
}

//////////////////////////////////////////////////
O3deRenderEngine *O3deRenderEngine::Instance()
{
  return gz::common::SingletonT<O3deRenderEngine>::Instance();
}

//////////////////////////////////////////////////
// Register this plugin
GZ_ADD_PLUGIN(rendering::O3deRenderEnginePlugin,
              rendering::RenderEnginePlugin)
GZ_ADD_PLUGIN_ALIAS(rendering::O3deRenderEnginePlugin,
                    "gz::rendering::o3de::Plugin")
