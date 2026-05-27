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
#ifndef GZ_RENDERING_O3DE_O3DESCENE_HH_
#define GZ_RENDERING_O3DE_O3DESCENE_HH_

#include <string>

#include "gz/rendering/base/BaseScene.hh"
#include "gz/rendering/o3de/O3deGeometry.hh"
#include "gz/rendering/o3de/O3deRenderTypes.hh"
#include "gz/rendering/o3de/Export.hh"

namespace gz
{
  namespace rendering
  {
    inline namespace GZ_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief O3DE implementation of the Scene class.
    ///
    /// M0 stub: this implements the full BaseScene factory surface so the
    /// scene graph (root visual, camera, primitive visuals, materials,
    /// lights) can be created and tracked, but nothing is wired into Atom
    /// yet. Unsupported object types return nullptr.
    class GZ_RENDERING_O3DE_VISIBLE O3deScene :
      public BaseScene
    {
      /// \brief Constructor
      /// \param[in] _id Unique scene Id
      /// \param[in] _name Scene name
      protected: O3deScene(unsigned int _id, const std::string &_name);

      /// \brief Destructor
      public: virtual ~O3deScene();

      // Documentation inherited.
      public: virtual void Fini() override;

      // Documentation inherited.
      public: virtual RenderEngine *Engine() const override;

      // Documentation inherited.
      public: virtual VisualPtr RootVisual() const override;

      // Documentation inherited.
      public: virtual math::Color AmbientLight() const override;

      // Documentation inherited.
      public: virtual void SetAmbientLight(const math::Color &_color) override;

      // Documentation inherited.
      protected: virtual bool LoadImpl() override;

      // Documentation inherited.
      protected: virtual bool InitImpl() override;

      // Documentation inherited.
      protected: virtual LightStorePtr Lights() const override;

      // Documentation inherited.
      protected: virtual SensorStorePtr Sensors() const override;

      // Documentation inherited.
      protected: virtual VisualStorePtr Visuals() const override;

      // Documentation inherited.
      protected: virtual MaterialMapPtr Materials() const override;

      // Documentation inherited.
      protected: virtual DirectionalLightPtr CreateDirectionalLightImpl(
                     unsigned int _id, const std::string &_name) override;

      // Documentation inherited.
      protected: virtual PointLightPtr CreatePointLightImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual SpotLightPtr CreateSpotLightImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual CameraPtr CreateCameraImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual DepthCameraPtr CreateDepthCameraImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual VisualPtr CreateVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual ArrowVisualPtr CreateArrowVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual AxisVisualPtr CreateAxisVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual COMVisualPtr CreateCOMVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual InertiaVisualPtr CreateInertiaVisualImpl(
                     unsigned int _id, const std::string &_name) override;

      // Documentation inherited.
      protected: virtual JointVisualPtr CreateJointVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual LightVisualPtr CreateLightVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GeometryPtr CreateBoxImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GeometryPtr CreateConeImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GeometryPtr CreateCylinderImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GeometryPtr CreatePlaneImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GeometryPtr CreateSphereImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual MeshPtr CreateMeshImpl(unsigned int _id,
                     const std::string &_name,
                     const MeshDescriptor &_desc) override;

      // Documentation inherited.
      protected: virtual CapsulePtr CreateCapsuleImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual GridPtr CreateGridImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual MarkerPtr CreateMarkerImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual LidarVisualPtr CreateLidarVisualImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual FrustumVisualPtr CreateFrustumVisualImpl(
                     unsigned int _id, const std::string &_name) override;

      // Documentation inherited.
      protected: virtual HeightmapPtr CreateHeightmapImpl(unsigned int _id,
                     const std::string &_name,
                     const HeightmapDescriptor &_desc) override;

      // Documentation inherited.
      protected: virtual WireBoxPtr CreateWireBoxImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual MaterialPtr CreateMaterialImpl(unsigned int _id,
                     const std::string &_name) override;

      // Documentation inherited.
      protected: virtual RenderTexturePtr CreateRenderTextureImpl(
                     unsigned int _id, const std::string &_name) override;

      // Documentation inherited.
      protected: virtual RenderWindowPtr CreateRenderWindowImpl(
                     unsigned int _id, const std::string &_name) override;

      // Documentation inherited.
      protected: virtual RayQueryPtr CreateRayQueryImpl(
                     unsigned int _id, const std::string &_name) override;

      /// \brief Helper used by the primitive factories to create a
      /// geometry object (box, sphere, cylinder, ...).
      /// \param[in] _id Unique object id
      /// \param[in] _name Object name
      /// \param[in] _type Primitive type to tag the geometry with
      /// \return Pointer to the created geometry
      protected: virtual GeometryPtr CreateGeometryImpl(unsigned int _id,
                     const std::string &_name,
                     O3deGeometry::GeometryType _type =
                         O3deGeometry::GeometryType::OTHER);

      /// \brief Initialize an o3de object: assigns id/name/scene and calls
      /// Load()/Init().
      /// \param[in] _object Object to initialize
      /// \param[in] _id Unique object id
      /// \param[in] _name Object name
      /// \return True on success
      protected: virtual bool InitObject(O3deObjectPtr _object,
                     unsigned int _id, const std::string &_name);

      /// \brief Create the root visual of the scene
      private: void CreateRootVisual();

      /// \brief Create the storage objects
      private: void CreateStores();

      /// \brief Create a shared pointer to self
      private: O3deScenePtr SharedThis();

      /// \brief Ambient light color
      protected: math::Color ambientLight;

      /// \brief Root visual of the scene
      protected: O3deVisualPtr rootVisual;

      /// \brief Store of sensors (e.g. cameras)
      protected: O3deSensorStorePtr sensors;

      /// \brief Store of visuals
      protected: O3deVisualStorePtr visuals;

      /// \brief Store of lights
      protected: O3deLightStorePtr lights;

      /// \brief Map of materials
      protected: O3deMaterialMapPtr materials;

      /// \brief Make the render engine our friend so it can create scenes
      private: friend class O3deRenderEngine;
    };
    }
  }
}
#endif
