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
#include "gz/rendering/o3de/O3deCamera.hh"
#include "gz/rendering/o3de/O3deRenderTarget.hh"

using namespace gz;
using namespace rendering;

//////////////////////////////////////////////////
O3deCamera::O3deCamera()
{
}

//////////////////////////////////////////////////
O3deCamera::~O3deCamera()
{
}

//////////////////////////////////////////////////
void O3deCamera::Render()
{
  if (this->renderTexture)
    this->renderTexture->Render();
}

//////////////////////////////////////////////////
RenderTargetPtr O3deCamera::RenderTarget() const
{
  return this->renderTexture;
}

//////////////////////////////////////////////////
void O3deCamera::Init()
{
  BaseCamera::Init();
  this->CreateRenderTexture();
}

//////////////////////////////////////////////////
void O3deCamera::CreateRenderTexture()
{
  this->renderTexture = O3deRenderTargetPtr(new O3deRenderTarget);
  // Let the target read this camera's pose/projection and scene at Copy() time.
  this->renderTexture->camera = this;
}
