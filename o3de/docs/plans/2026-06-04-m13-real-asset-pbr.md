# M13 — Real-asset PBR Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Real-world mesh assets (jetty_demo GLB models with embedded PBR texture sets, textured .dae) render with their own file materials — factors + albedo/normal/metalness/roughness maps — auto-applied by the o3de backend, with no gz-API material call.

**Architecture:** Backend-side extraction (Approach A, ogre2 precedent): `RegisterMesh` extracts the mesh file's material[0] into a plain `MeshFileMaterialCpu` struct on the gz thread (next to the existing `MeshGeometryCpu`); `SubmitMeshes` applies it on the render thread when the per-mesh snapshot carries the M12 sentinels (no explicit gz material). Texture upload generalizes the M11-D path into `StreamingImageFromCommonImage(img, srgb)` with sRGB/linear split and per-source caches.

**Tech Stack:** C++17, gz-rendering plugin (`o3de/`), Atom RPI (StandardPBR material, StreamingImage), gz-common graphics (Mesh/SubMesh/Material/Pbr/Image).

**Spec:** `o3de/docs/specs/2026-06-04-m13-real-asset-pbr-design.md` (approved, commit `95a3ffb0`).

---

## Project context the engineer must know

- **Repo / branch:** `/home/jrivero/code/gz/gz-rendering`, branch `o3de_render_engine_poc`. Everything M13 touches lives in `o3de/`.
- **No unit-test harness for the backend.** This PoC verifies by building driver examples, running them through the CPU-readback capture path, and checking telemetry lines + output PNGs. Each task below ends with a concrete build+run verification — treat those as the tests.
- **Build:** NEVER `--target o3de` (silent no-op). Always:

  ```bash
  cd /home/jrivero/code/gz/gz-rendering/build_o3de
  cmake --build . --target gz-rendering-o3de -j5
  SRC="lib/libgz-rendering-o3de.so.11.0.0~pre1"
  WS=/home/jrivero/code/gz/ws_o3de_rendering/install/lib
  cp -f "$SRC" "$WS/libgz-rendering-o3de.so.11.0.0~pre1"
  cp -f "$SRC" "$WS/gz-rendering/engine-plugins/libgz-rendering-o3de.so.11.0.0~pre1"
  ```

  Verify freshness when in doubt: `grep -c "M13 file-material" "$WS/libgz-rendering-o3de.so.11.0.0~pre1"` (≥1 after Task 3).
- **Commits:** use `git ci` (user alias), from the repo root (NOT from `build_o3de` or `/tmp`). Trailer on every commit:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- **Threading rule:** `RegisterMesh` runs on the gz thread — gz-common calls only, no Atom. All Atom calls (StreamingImage creation, material property sets) happen render-side in `SubmitMeshes`. The `Impl::mutex` guards the gz↔render staging maps.
- **M12 sentinel contract** (`o3de/src/O3deBackend.hh:175` `O3deMeshData`): `metallic = -1`, `roughness = -1`, `texturePath = ""` mean "no gz material attached". When a gz material IS attached, `GatherFrame` sets all three unconditionally (so `metallic >= 0` always holds then).
- **Key existing code anchors** (all in `o3de/src/O3deBackend.cc` unless noted):
  - `struct MeshGeometryCpu` — line ~261 (file-scope, before `class O3deBackend::Impl`).
  - `Impl::meshGeometry` map + `meshUnregister` — line ~468 (mutex-guarded).
  - `Impl::fileTextures` + `FileBaseColorImage()` decl — line ~494.
  - `FileBaseColorImage()` impl — line ~2221 (M11-D: decode via `gz::common::Image`, `CreateFromCpuData`, `R8G8B8A8_UNORM_SRGB`, per-path cache incl. cached failures).
  - `SubmitMeshes()` — line ~2268; unregister drop block ~2281; per-mesh material creation block ~2389–2483 (baseColor.color at 2396, roughness.factor 2409, metallic.factor 2417, M11-B/D texture block 2429–2444, emissive crutch 2453, `material->Compile()` 2474, telemetry fprintf 2475).
  - `ExtractMeshGeometry(const gz::common::Mesh*)` — line ~1714 (file-scope helper in the same scope; M13's extractor goes next to it).
  - `RegisterMesh` / `UnregisterMesh` — lines ~4084 / ~4106.
  - gz-common includes already present at lines 143–146: `gz/common/Image.hh`, `Mesh.hh`, `SubMesh.hh`, `MeshManager.hh`.
- **gz-common API facts** (verified in `~/code/gz/gz-common/graphics/include/gz/common/`):
  - `Mesh::MaterialCount()`, `Mesh::MaterialByIndex(unsigned int)` → `MaterialPtr` (= `std::shared_ptr<Material>`).
  - `SubMesh::GetMaterialIndex()` → `std::optional<unsigned int>`.
  - `Material::Diffuse()` → `math::Color`; `Material::TextureData()` → `std::shared_ptr<const Image>` (in-memory albedo — this is where AssimpLoader puts GLB embedded base color); `Material::TextureImage()` → `std::string` (texture name/path — for .dae this is a resolvable file path; for GLB in-memory it may be a non-file name, which is why the in-memory form takes precedence).
  - `Material::PbrMaterial()` → `Pbr*` (may be null, e.g. plain .dae).
  - `Pbr::Metalness()/Roughness()` → `double`; `Pbr::AlbedoMap()/NormalMap()/MetalnessMap()/RoughnessMap()` → `std::string`; `Pbr::NormalMapData()/MetalnessMapData()/RoughnessMapData()` → `std::shared_ptr<const Image>`; `Pbr::NormalMapType()` → `NormalMapSpace` (enum class, `TANGENT = 0`). AssimpLoader produces TANGENT-space normal maps and pre-splits the combined glTF metallic-roughness texture into separate images. There is NO `AlbedoMapData()` on Pbr — in-memory albedo is `Material::TextureData()`.
- **Test assets** (out-of-repo, never commit binaries):
  - GLB with embedded PBR set: `/home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb` (15,777 verts; renders GREY before M13 — reference at `/tmp/glb_smoke_forklift.png`).
  - .dae with sidecar texture file: `/home/jrivero/code/gz/jetty_demo/jetty_demo/models/cordless_drill_default_inertia/meshes/cordless_drill.dae` (references `../materials/textures/cordless_drill.png`).
- **Scratch to promote:** `/tmp/o3de_debug/glb_smoke/{Main.cc,CMakeLists.txt}` (uncommitted) becomes `o3de/examples/mesh_pbr_viewer/` in Task 4. Full adapted code is inlined in Task 4 — you do not need the scratch files.

---

### Task 1: `MeshFileMaterialCpu` extraction at RegisterMesh (gz thread)

**Files:**
- Modify: `o3de/src/O3deBackend.cc` (includes ~line 146; struct after `MeshGeometryCpu` ~line 277; extractor next to `ExtractMeshGeometry` ~line 1714; `Impl` members ~line 470; `RegisterMesh` ~4084; `UnregisterMesh` ~4106; `SubmitMeshes` drop block ~2290)

- [ ] **Step 1: Add the missing gz-common includes**

After line 146 (`#include <gz/common/MeshManager.hh>`) add:

```cpp
#include <gz/common/Material.hh>
#include <gz/common/Pbr.hh>
```

Also confirm `<memory>` and `<map>` are included near the std includes (lines 50–58); add whichever is missing (Task 2 needs `<map>`, this task needs `<memory>` for `std::shared_ptr` — `<memory>` may already come in transitively, include it explicitly anyway).

- [ ] **Step 2: Add the `MeshFileMaterialCpu` struct**

Immediately after the closing `};` of `struct MeshGeometryCpu` (~line 277), at the same file scope:

```cpp
/// \brief M13: the material a mesh FILE carries (GLB embedded PBR texture
/// set, .dae sidecar textures), extracted once on the gz thread at
/// RegisterMesh and consumed on the render thread when the per-mesh
/// StandardPBR instance is built. Plain data + gz-common image handles --
/// no Atom/AzCore types, safe to build off-thread. Applied only when the
/// frame snapshot carries the M12 sentinels (no explicit gz material);
/// an explicitly-set gz material wins ENTIRELY (no mixing).
struct MeshFileMaterialCpu
{
  bool present = false;
  float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};  //!< common::Material diffuse.
  float metalness = 0.0f;  //!< Pbr metallic factor (0 when no Pbr block).
  float roughness = 1.0f;  //!< Pbr roughness factor (1 when no Pbr block).
  // In-memory images (GLB embedded textures; null when absent). Albedo
  // comes from common::Material::TextureData(); the other three from the
  // Pbr block's *MapData(). gz-common pre-splits the combined glTF
  // metallic-roughness texture into separate images.
  std::shared_ptr<const gz::common::Image> albedoImg, normalImg,
                                           metalnessImg, roughnessImg;
  // File-path forms (.dae sidecar textures; empty when absent). The
  // in-memory form takes precedence at apply time: for GLB the "name" in
  // TextureImage() is not a resolvable file.
  std::string albedoPath, normalPath, metalnessPath, roughnessPath;
};
```

- [ ] **Step 3: Add the extractor**

Immediately after the closing brace of `ExtractMeshGeometry` (find `MeshGeometryCpu ExtractMeshGeometry(const gz::common::Mesh *_mesh)` at ~1714 and its end), in the same scope and with the same 2-space indentation style:

```cpp
  /// \brief M13: extract the mesh file's material[0] into plain data on the
  /// gz thread (no Atom calls). Selection: the first submesh that names a
  /// material index picks it; otherwise material 0. Multi-material meshes
  /// get material[0] applied to the whole flattened mesh (logged once here;
  /// per-submesh material slots are a future milestone).
  MeshFileMaterialCpu ExtractMeshFileMaterial(const gz::common::Mesh *_mesh)
  {
    MeshFileMaterialCpu out;
    if (_mesh == nullptr || _mesh->MaterialCount() == 0u)
      return out;

    if (_mesh->MaterialCount() > 1u)
    {
      std::fprintf(stderr,
          "[gz-o3de] M13 mesh '%s' has %u materials; applying material[0] "
          "to the whole mesh (per-submesh materials not supported yet)\n",
          _mesh->Name().c_str(), _mesh->MaterialCount());
    }

    gz::common::MaterialPtr mat;
    for (unsigned int s = 0; s < _mesh->SubMeshCount() && !mat; ++s)
    {
      auto subMesh = _mesh->SubMeshByIndex(s).lock();
      if (!subMesh)
        continue;
      if (auto idx = subMesh->GetMaterialIndex())
        mat = _mesh->MaterialByIndex(*idx);
    }
    if (!mat)
      mat = _mesh->MaterialByIndex(0u);
    if (!mat)
      return out;

    out.present = true;
    const gz::math::Color c = mat->Diffuse();
    out.color[0] = c.R();
    out.color[1] = c.G();
    out.color[2] = c.B();
    out.color[3] = c.A();

    // Albedo lives on common::Material itself (AssimpLoader stores the GLB
    // embedded base-color via SetTextureImage; ColladaLoader stores a path).
    out.albedoImg = mat->TextureData();
    out.albedoPath = mat->TextureImage();

    if (const gz::common::Pbr *pbr = mat->PbrMaterial())
    {
      out.metalness = static_cast<float>(pbr->Metalness());
      out.roughness = static_cast<float>(pbr->Roughness());
      if (!out.albedoImg && out.albedoPath.empty())
        out.albedoPath = pbr->AlbedoMap();
      if (pbr->NormalMapType() == gz::common::NormalMapSpace::TANGENT)
      {
        out.normalImg = pbr->NormalMapData();
        out.normalPath = pbr->NormalMap();
      }
      else if (pbr->NormalMapData() || !pbr->NormalMap().empty())
      {
        std::fprintf(stderr,
            "[gz-o3de] M13 mesh '%s': non-TANGENT normal map skipped\n",
            _mesh->Name().c_str());
      }
      out.metalnessImg = pbr->MetalnessMapData();
      out.metalnessPath = pbr->MetalnessMap();
      out.roughnessImg = pbr->RoughnessMapData();
      out.roughnessPath = pbr->RoughnessMap();
    }
    return out;
  }
```

- [ ] **Step 4: Add the staging map to `Impl`**

Right after the `meshGeometry` / `meshUnregister` members (~line 470):

```cpp
  // M13: the mesh FILE's material, extracted at RegisterMesh, keyed by gz
  // id. Guarded by `mutex` (gz thread writes, render thread reads at
  // first-sight material creation). Only ids whose mesh actually carries a
  // material have an entry.
  public: std::unordered_map<uint64_t, MeshFileMaterialCpu> meshFileMaterials;
```

- [ ] **Step 5: Wire RegisterMesh / UnregisterMesh / the render-side drop**

`RegisterMesh` (~4084) becomes:

```cpp
void O3deBackend::RegisterMesh(uint64_t _id, const gz::common::Mesh *_mesh)
{
  Impl &d = *this->dataPtr;
  // Extract on the caller's (gz) thread -- no Atom calls -- then hand the plain
  // geometry to the render thread, which builds the Atom model lazily. A null
  // or geometry-less mesh is treated as an unregister.
  MeshGeometryCpu geom = ExtractMeshGeometry(_mesh);
  MeshFileMaterialCpu fileMat = ExtractMeshFileMaterial(_mesh);
  std::lock_guard<std::mutex> lock(d.mutex);
  if (geom.Empty())
  {
    d.meshGeometry.erase(_id);
    d.meshFileMaterials.erase(_id);
    d.meshUnregister.push_back(_id);
    return;
  }
  // Each mesh has a unique gz id and is registered once (CreateMeshImpl), so we
  // just stage the geometry; the render thread builds the model on first sight.
  // Re-registering the same id with new geometry is not a supported case here
  // (it would keep the already-built model); UnregisterMesh first if needed.
  d.meshGeometry[_id] = std::move(geom);
  if (fileMat.present)
    d.meshFileMaterials[_id] = std::move(fileMat);
  else
    d.meshFileMaterials.erase(_id);
}
```

`UnregisterMesh` (~4106): add `d.meshFileMaterials.erase(_id);` next to the existing `d.meshGeometry.erase(_id);`.

`SubmitMeshes` unregister drop block (~2290, inside the `for (uint64_t id : toDrop)` loop): the existing code takes `lock(this->mutex)` just for `this->meshGeometry.erase(id);` — extend that locked statement to also erase the file material:

```cpp
      std::lock_guard<std::mutex> lock(this->mutex);
      this->meshGeometry.erase(id);
      this->meshFileMaterials.erase(id);
```

- [ ] **Step 6: Build and regression-run**

```bash
cd /home/jrivero/code/gz/gz-rendering/build_o3de
cmake --build . --target gz-rendering-o3de -j5
SRC="lib/libgz-rendering-o3de.so.11.0.0~pre1"
WS=/home/jrivero/code/gz/ws_o3de_rendering/install/lib
cp -f "$SRC" "$WS/libgz-rendering-o3de.so.11.0.0~pre1"
cp -f "$SRC" "$WS/gz-rendering/engine-plugins/libgz-rendering-o3de.so.11.0.0~pre1"
/home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials/pbr_materials.sh /tmp/m13_t1_regression.png
```

Expected: clean build; pbr_materials exits 0, prints `[glb… wrote /tmp/m13_t1_regression.png`-style success (its own `[pbr_materials] wrote …` line), and the PNG visually matches `o3de/docs/screenshots/m12-pbr-materials-gz-api.png` (extraction runs but nothing consumes it yet — zero behavior change).

- [ ] **Step 7: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/src/O3deBackend.cc
git ci -m "o3de: M13 extract mesh-file material at RegisterMesh

MeshFileMaterialCpu (factors + 4 map sources, in-memory and path forms)
extracted from material[0] on the gz thread, staged per mesh id under the
snapshot mutex. Not consumed yet -- application lands with the render-side
half.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `StreamingImageFromCommonImage` helper + sRGB-keyed texture caches (render thread)

**Files:**
- Modify: `o3de/src/O3deBackend.cc` (`Impl` members ~line 490; `FileBaseColorImage` impl ~2221; its call site ~2432)

- [ ] **Step 1: Replace the texture-cache members and declarations in `Impl`**

Replace the existing block at ~line 492–496:

```cpp
  public: std::unordered_map<std::string,
      AZ::Data::Instance<AZ::RPI::StreamingImage>> fileTextures;
  public: AZ::Data::Instance<AZ::RPI::StreamingImage> FileBaseColorImage(
      const std::string &_path);
```

with:

```cpp
  // M11 Phase D / M13: textures decoded with gz::common::Image and uploaded
  // once to Atom, cached so a mesh seen every frame uploads only on first
  // sight. Two sources, each keyed WITH the srgb flag (albedo is sRGB;
  // normal/metalness/roughness data is linear -- uploading those as sRGB is
  // the classic washed-out-normals bug):
  //  - fileTextures: by file path (.dae sidecar textures, gz-API texture
  //    files). Failures are cached as null so a bad path logs once.
  //  - memTextures: by image object address (GLB embedded textures). The
  //    shared_ptr in meshFileMaterials keeps the image alive while its mesh
  //    is registered, so the key cannot dangle while cached entries are
  //    reachable; a recycled address after unregister could at worst serve
  //    a stale texture to a brand-new image (accepted PoC risk).
  public: std::map<std::pair<std::string, bool>,
      AZ::Data::Instance<AZ::RPI::StreamingImage>> fileTextures;
  public: std::map<std::pair<const void *, bool>,
      AZ::Data::Instance<AZ::RPI::StreamingImage>> memTextures;
  public: AZ::Data::Instance<AZ::RPI::StreamingImage>
      StreamingImageFromCommonImage(const gz::common::Image &_img, bool _srgb);
  public: AZ::Data::Instance<AZ::RPI::StreamingImage> FileTexture(
      const std::string &_path, bool _srgb);
  public: AZ::Data::Instance<AZ::RPI::StreamingImage> MemTexture(
      const std::shared_ptr<const gz::common::Image> &_img, bool _srgb);
```

- [ ] **Step 2: Replace the `FileBaseColorImage` implementation (~2221–2265) with the three functions**

Keep the M11-D comment block above it, updated. New code:

```cpp
//////////////////////////////////////////////////
// M11 Phase D / M13: upload a decoded gz::common::Image to Atom as a
// StreamingImage. _srgb selects R8G8B8A8_UNORM_SRGB (albedo/base-color) vs
// R8G8B8A8_UNORM (linear data: normal, metalness, roughness maps). Must run
// on the render thread. Returns null on any failure (caller falls back).
AZ::Data::Instance<AZ::RPI::StreamingImage>
O3deBackend::Impl::StreamingImageFromCommonImage(
    const gz::common::Image &_img, bool _srgb)
{
  AZ::Data::Instance<AZ::RPI::StreamingImage> result;  // null until built
  if (!_img.Valid())
    return result;
  const std::vector<unsigned char> rgba = _img.RGBAData();
  const uint32_t w = _img.Width();
  const uint32_t h = _img.Height();
  if (rgba.size() < static_cast<size_t>(w) * h * 4u || w == 0u || h == 0u)
    return result;

  auto *imageSystem = AZ::RPI::ImageSystemInterface::Get();
  const AZ::Data::Instance<AZ::RPI::StreamingImagePool> pool =
      imageSystem ? imageSystem->GetSystemStreamingPool()
                  : AZ::Data::Instance<AZ::RPI::StreamingImagePool>();
  if (pool)
  {
    result = AZ::RPI::StreamingImage::CreateFromCpuData(
        *pool, AZ::RHI::ImageDimension::Image2D, AZ::RHI::Size(w, h, 1u),
        _srgb ? AZ::RHI::Format::R8G8B8A8_UNORM_SRGB
              : AZ::RHI::Format::R8G8B8A8_UNORM,
        rgba.data(), rgba.size());
  }
  return result;
}

//////////////////////////////////////////////////
// Decode a texture FILE and upload it (cached per path+srgb, failures
// cached as null so a bad path logs once). This is the path a gz material's
// Texture() or a .dae sidecar texture takes.
AZ::Data::Instance<AZ::RPI::StreamingImage> O3deBackend::Impl::FileTexture(
    const std::string &_path, bool _srgb)
{
  const auto key = std::make_pair(_path, _srgb);
  auto cached = this->fileTextures.find(key);
  if (cached != this->fileTextures.end())
    return cached->second;

  gz::common::Image img(_path);
  AZ::Data::Instance<AZ::RPI::StreamingImage> result =
      img.Valid() ? this->StreamingImageFromCommonImage(img, _srgb)
                  : AZ::Data::Instance<AZ::RPI::StreamingImage>();
  std::fprintf(stderr,
      "[gz-o3de] texture file %s (srgb=%d) -> %s\n",
      _path.c_str(), _srgb ? 1 : 0, result ? "READY" : "FAILED");
  this->fileTextures[key] = result;
  return result;
}

//////////////////////////////////////////////////
// M13: upload an IN-MEMORY gz::common::Image (GLB embedded texture, already
// decoded by the mesh loader), cached per image address+srgb.
AZ::Data::Instance<AZ::RPI::StreamingImage> O3deBackend::Impl::MemTexture(
    const std::shared_ptr<const gz::common::Image> &_img, bool _srgb)
{
  if (!_img)
    return {};
  const auto key = std::make_pair(
      static_cast<const void *>(_img.get()), _srgb);
  auto cached = this->memTextures.find(key);
  if (cached != this->memTextures.end())
    return cached->second;

  AZ::Data::Instance<AZ::RPI::StreamingImage> result =
      this->StreamingImageFromCommonImage(*_img, _srgb);
  std::fprintf(stderr,
      "[gz-o3de] M13 in-memory texture %ux%u (srgb=%d) -> %s\n",
      _img->Width(), _img->Height(), _srgb ? 1 : 0,
      result ? "READY" : "FAILED");
  this->memTextures[key] = result;
  return result;
}
```

- [ ] **Step 3: Update the single call site**

At ~2432 (M11 B/D texture block in `SubmitMeshes`) replace
`this->FileBaseColorImage(m.texturePath)` with
`this->FileTexture(m.texturePath, true)`.

Then `grep -n "FileBaseColorImage" o3de/src/O3deBackend.cc` — expected: no matches remain.

- [ ] **Step 4: Build and regression-run**

Same build+copy block as Task 1 Step 6, then:

```bash
/home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials/pbr_materials.sh /tmp/m13_t2_regression.png 2>&1 | grep -E "texture file|wrote"
```

Expected: a `[gz-o3de] texture file .../gz_albedo_demo.png (srgb=1) -> READY` line (exercises the renamed path-cache function) and the `wrote` line; PNG visually identical to the M12 reference (textured sphere still shows the gz albedo demo texture).

- [ ] **Step 5: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/src/O3deBackend.cc
git ci -m "o3de: M13 StreamingImageFromCommonImage + srgb-keyed texture caches

Generalize the M11-D albedo-file upload into one helper with an explicit
sRGB/linear format choice, a path+srgb file cache (FileTexture, replacing
FileBaseColorImage) and an address+srgb in-memory cache (MemTexture) for
GLB embedded textures.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Apply the file material in SubmitMeshes (render thread)

**Files:**
- Modify: `o3de/src/O3deBackend.cc` (`SubmitMeshes` per-mesh material block; insert between the M11 B/D texture block closing `}` (~2444) and the emissive-crutch comment (~2446))

- [ ] **Step 1: Insert the application block**

The insertion point is inside `else if (this->meshMaterialAsset.IsReady()) { ... if (material) { ... } }`, after the `if (!m.texturePath.empty() || m.textured) { ... }` block and before the `// Emissive visibility workaround` comment. Insert:

```cpp
          // M13: no explicit gz material on this mesh (all-sentinel
          // snapshot: GatherFrame sets metallic/roughness/texturePath
          // unconditionally when a gz material is attached) -> auto-apply
          // the material the mesh FILE carries, extracted at RegisterMesh.
          // An explicitly-set gz material wins ENTIRELY (no mixing).
          const bool explicitMat = m.metallic >= 0.0f ||
              m.roughness >= 0.0f || !m.texturePath.empty();
          if (!explicitMat)
          {
            MeshFileMaterialCpu fm;
            {
              std::lock_guard<std::mutex> lock(this->mutex);
              auto fIt = this->meshFileMaterials.find(m.id);
              if (fIt != this->meshFileMaterials.end())
                fm = fIt->second;  // cheap: floats + shared_ptrs
            }
            if (fm.present)
            {
              const auto fcIdx =
                  material->FindPropertyIndex(AZ::Name("baseColor.color"));
              if (fcIdx.IsValid())
                material->SetPropertyValue(fcIdx, AZ::Color(
                    fm.color[0], fm.color[1], fm.color[2], fm.color[3]));
              const auto fmIdx =
                  material->FindPropertyIndex(AZ::Name("metallic.factor"));
              if (fmIdx.IsValid())
                material->SetPropertyValue(fmIdx,
                    std::clamp(fm.metalness, 0.0f, 1.0f));
              const auto frIdx =
                  material->FindPropertyIndex(AZ::Name("roughness.factor"));
              if (frIdx.IsValid())
                material->SetPropertyValue(frIdx,
                    std::clamp(fm.roughness, 0.0f, 1.0f));

              // One map = one StandardPBR property group. Prefer the
              // in-memory image (GLB embedded), else the file path (.dae).
              // Returns the source tag for the telemetry line.
              auto bindMap = [&](const char *_group,
                  const std::shared_ptr<const gz::common::Image> &_img,
                  const std::string &_path, bool _srgb) -> const char *
              {
                AZ::Data::Instance<AZ::RPI::StreamingImage> tex;
                const char *src = "none";
                if (_img)
                {
                  tex = this->MemTexture(_img, _srgb);
                  src = "mem";
                }
                else if (!_path.empty())
                {
                  tex = this->FileTexture(_path, _srgb);
                  src = "path";
                }
                else
                {
                  return src;  // map not present on this material
                }
                if (!tex)
                  return "fail";  // decode/upload failed (already logged)
                const auto texIdx = material->FindPropertyIndex(AZ::Name(
                    AZStd::string::format("%s.textureMap", _group)));
                const auto useIdx = material->FindPropertyIndex(AZ::Name(
                    AZStd::string::format("%s.useTexture", _group)));
                if (!texIdx.IsValid() || !useIdx.IsValid())
                  return "fail";
                material->SetPropertyValue(texIdx,
                    AZ::Data::Instance<AZ::RPI::Image>(tex));
                material->SetPropertyValue(useIdx, true);
                return src;
              };
              const char *aSrc =
                  bindMap("baseColor", fm.albedoImg, fm.albedoPath, true);
              const char *nSrc =
                  bindMap("normal", fm.normalImg, fm.normalPath, false);
              const char *mSrc = bindMap("metallic",
                  fm.metalnessImg, fm.metalnessPath, false);
              const char *rSrc = bindMap("roughness",
                  fm.roughnessImg, fm.roughnessPath, false);
              std::fprintf(stderr,
                  "[gz-o3de] M13 file-material id=%llu albedo=%s normal=%s "
                  "metal=%s rough=%s factors m=%.2f r=%.2f\n",
                  static_cast<unsigned long long>(m.id),
                  aSrc, nSrc, mSrc, rSrc, fm.metalness, fm.roughness);
            }
          }
```

Notes for the implementer:
- This runs once per mesh id (first-sight material creation), so the telemetry line is naturally one-shot.
- `std::clamp` is already used in this function (lines ~2412/2420); `<algorithm>` is included.
- `AZStd::string::format` is already used in this function (~2334).

- [ ] **Step 2: Build, copy, freshness-check**

Same build+copy block as Task 1 Step 6, then:

```bash
strings "/home/jrivero/code/gz/ws_o3de_rendering/install/lib/libgz-rendering-o3de.so.11.0.0~pre1" | grep -c "M13 file-material"
```

Expected: `1` (or more).

- [ ] **Step 3: Smoke-run the existing scratch viewer to see M13 fire**

```bash
mkdir -p /tmp/o3de_debug/glb_smoke/build && cd /tmp/o3de_debug/glb_smoke/build && cmake .. >/dev/null && make >/dev/null
# run it with the pbr_materials environment (it sets plugin paths + IBL):
# easiest: temporarily reuse the launcher pattern --
GZ_O3DE_WS=/home/jrivero/code/gz/ws_o3de_rendering
source "$GZ_O3DE_WS/install/setup.bash"
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/home/jrivero/code/gz/gz-rendering/vendor/o3de/build/linux/bin/profile"
export GZ_O3DE_DEMO_IBL=1
./glb_smoke /home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb /tmp/m13_t3_forklift.png 2>&1 | grep -E "M13|wrote"
```

(If the scratch build dir no longer exists this step can be skipped — Task 4's example is the real driver; but running it now gives the fastest signal.)

Expected: `[gz-o3de] M13 file-material id=... albedo=mem normal=mem metal=mem rough=mem factors m=... r=...` for the forklift, `albedo=none/...` (no M13 line at all) for the ground box (it has an explicit gz material → sentinel check skips it), and a forklift that is no longer uniformly grey in `/tmp/m13_t3_forklift.png`.

- [ ] **Step 4: Regression-run pbr_materials**

```bash
/home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials/pbr_materials.sh /tmp/m13_t3_regression.png 2>&1 | grep -E "M13|wrote"
```

Expected: NO `M13 file-material` lines (every pbr_materials mesh has an explicit gz material; the primitive meshes carry no file material anyway), PNG matches the M12 reference.

- [ ] **Step 5: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/src/O3deBackend.cc
git ci -m "o3de: M13 auto-apply mesh-file materials in SubmitMeshes

When a mesh's frame snapshot carries the M12 sentinels (no explicit gz
material), apply the file material extracted at RegisterMesh: diffuse +
metallic/roughness factors and the albedo (sRGB) / normal / metalness /
roughness (linear) maps, in-memory form preferred over path form. Explicit
gz materials win entirely. One-shot telemetry per mesh.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `mesh_pbr_viewer` example (promote the glb_smoke scratch)

**Files:**
- Create: `o3de/examples/mesh_pbr_viewer/Main.cc`
- Create: `o3de/examples/mesh_pbr_viewer/CMakeLists.txt`
- Create: `o3de/examples/mesh_pbr_viewer/mesh_pbr_viewer.sh`
- Create: `o3de/examples/mesh_pbr_viewer/README.md`

- [ ] **Step 1: Write `Main.cc`**

```cpp
// M13: view a real mesh asset (GLB/.dae/.obj...) with the material the FILE
// carries, through the public gz-rendering API on the o3de backend -- no
// SetMaterial call, so the backend's mesh-file material auto-apply (M13) is
// what colours it. Renders headless via the CPU-readback path and writes a
// PNG. Promoted from the M13 smoke test that proved the geometry path on the
// jetty Forklift GLB.
//
// Usage: mesh_pbr_viewer <mesh-file> [output.png]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <thread>

#include <gz/common/Image.hh>
#include <gz/common/Mesh.hh>
#include <gz/common/MeshManager.hh>

#include <gz/rendering/Camera.hh>
#include <gz/rendering/Image.hh>
#include <gz/rendering/Light.hh>
#include <gz/rendering/Material.hh>
#include <gz/rendering/Mesh.hh>
#include <gz/rendering/MeshDescriptor.hh>
#include <gz/rendering/RenderEngine.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/Scene.hh>
#include <gz/rendering/Visual.hh>

using namespace gz;
using namespace gz::rendering;

int main(int _argc, char **_argv)
{
  if (_argc < 2)
  {
    std::fprintf(stderr, "usage: mesh_pbr_viewer <mesh-file> [out.png]\n");
    return 2;
  }
  const std::string meshPath = _argv[1];
  const std::string outPath =
      _argc > 2 ? _argv[2] : "/tmp/mesh_pbr_viewer.png";

  // Pre-load via gz-common directly so we can print bounds + submesh info
  // before the renderer touches it (and auto-frame the camera from bounds).
  auto *mgr = common::MeshManager::Instance();
  const common::Mesh *cm = mgr->Load(meshPath);
  if (!cm)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] gz-common FAILED to load %s\n",
        meshPath.c_str());
    return 1;
  }
  const math::Vector3d mn = cm->Min(), mx = cm->Max();
  std::fprintf(stderr,
      "[mesh_pbr_viewer] loaded: %u submeshes, %u verts, %u indices, "
      "%u materials\n  bounds min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f)\n",
      cm->SubMeshCount(), cm->VertexCount(), cm->IndexCount(),
      cm->MaterialCount(),
      mn.X(), mn.Y(), mn.Z(), mx.X(), mx.Y(), mx.Z());

  RenderEngine *engine = rendering::engine("o3de");
  if (!engine)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] engine 'o3de' not found\n");
    return 1;
  }
  ScenePtr scene = engine->CreateScene("scene");
  if (!scene)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] CreateScene failed\n");
    return 1;
  }
  scene->SetAmbientLight(0.3, 0.3, 0.3);
  VisualPtr root = scene->RootVisual();

  // The asset, by file path, centred on the origin, resting on the ground.
  // NO SetMaterial: the mesh file's own material must apply (M13).
  MeshDescriptor descriptor(meshPath);
  MeshPtr mesh = scene->CreateMesh(descriptor);
  if (!mesh)
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] CreateMesh(%s) FAILED\n",
        meshPath.c_str());
    return 1;
  }
  VisualPtr v = scene->CreateVisual("asset");
  v->AddGeometry(mesh);
  const math::Vector3d centre = (mn + mx) * 0.5;
  v->SetLocalPosition(-centre.X(), -centre.Y(), -mn.Z());
  root->AddChild(v);

  // Ground + the proven key/fill lighting from the pbr_materials example,
  // intensity and distances scaled by the model extent.
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.40, 0.42, 0.45);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.9f);
    MeshDescriptor desc("unit_box");
    MeshPtr g = scene->CreateMesh(desc);
    g->SetMaterial(mat);
    VisualPtr ground = scene->CreateVisual("ground");
    ground->AddGeometry(g);
    const double span = std::max({mx.X() - mn.X(), mx.Y() - mn.Y(), 4.0});
    ground->SetLocalScale(span * 4.0, span * 4.0, 0.5);
    ground->SetLocalPosition(0.0, 0.0, -0.25);
    root->AddChild(ground);
  }
  const double ext = std::max({mx.X() - mn.X(), mx.Y() - mn.Y(),
      mx.Z() - mn.Z()});
  {
    PointLightPtr key = scene->CreatePointLight();
    key->SetLocalPosition(-1.0 * ext, 1.2 * ext, 2.0 * ext);
    key->SetDiffuseColor(1.0, 0.97, 0.92);
    key->SetIntensity(220.0 * ext);
    key->SetAttenuationRange(20.0 * ext);
    root->AddChild(key);
    PointLightPtr fill = scene->CreatePointLight();
    fill->SetLocalPosition(-1.5 * ext, -1.0 * ext, 0.8 * ext);
    fill->SetDiffuseColor(0.6, 0.7, 0.95);
    fill->SetIntensity(70.0 * ext);
    fill->SetAttenuationRange(18.0 * ext);
    root->AddChild(fill);
  }

  // Camera auto-framed from the bounds: back off ~2.2x the largest extent,
  // looking at the model's mid-height.
  CameraPtr camera = scene->CreateCamera("camera");
  const double dist = 2.2 * ext;
  camera->SetLocalPosition(-dist, 0.6 * dist, 0.45 * ext);
  // yaw toward the model (it sits at the origin): atan2(-y, -x)
  const double yaw = std::atan2(-0.6 * dist, dist);
  camera->SetLocalRotation(0.0, 0.05, yaw);
  camera->SetImageWidth(1280);
  camera->SetImageHeight(720);
  camera->SetAspectRatio(1280.0 / 720.0);
  camera->SetHFOV(1.047);
  root->AddChild(camera);

  // 30-frame warmup capture. The buffer starts uninitialized heap memory and
  // Copy() leaves it untouched on failure -- zero it first so the all-zero
  // check below is a reliable "backend never rendered" detector.
  Image image = camera->CreateImage();
  const std::size_t bufSize = static_cast<std::size_t>(
      camera->ImageWidth()) * camera->ImageHeight() * 3u;
  std::memset(image.Data<unsigned char>(), 0, bufSize);
  for (int frame = 0; frame < 30; ++frame)
  {
    camera->Capture(image);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const unsigned char *buf = image.Data<unsigned char>();
  if (std::all_of(buf, buf + bufSize,
      [](unsigned char _b) { return _b == 0u; }))
  {
    std::fprintf(stderr, "[mesh_pbr_viewer] ERROR: backend never rendered\n");
    return 1;
  }
  common::Image out;
  out.SetFromData(image.Data<unsigned char>(),
      camera->ImageWidth(), camera->ImageHeight(), common::Image::RGB_INT8);
  out.SavePNG(outPath);
  std::printf("[mesh_pbr_viewer] wrote %s\n", outPath.c_str());
  // The backend exits via std::quick_exit, which skips stdio flushing.
  std::fflush(stdout);
  return 0;
}
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(mesh-pbr-viewer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gz-rendering REQUIRED)

add_executable(mesh_pbr_viewer Main.cc)

target_link_libraries(mesh_pbr_viewer
  ${GZ-RENDERING_LIBRARIES}
)
```

- [ ] **Step 3: Write `mesh_pbr_viewer.sh`** (mirrors `pbr_materials.sh`)

```bash
#!/usr/bin/env bash
#
# M13: view a real mesh asset with the material the FILE carries (GLB
# embedded PBR texture sets, .dae sidecar textures) -- the backend
# auto-applies it; the example makes no SetMaterial call.
#
# Renders a headless offscreen scene via the CPU-readback path (no Qt, no
# zero-copy interop) and writes a PNG.
#
# Usage: mesh_pbr_viewer.sh <mesh-file> [output.png]
#                                       (default /tmp/mesh_pbr_viewer.png)
#
# Env overrides (same as live_demo.sh):
#   GZ_O3DE_WS    colcon workspace whose install/ holds gz-rendering
#   GZ_O3DE_REPO  gz-rendering source checkout (for vendor/o3de runtime libs)

set -euo pipefail

[ $# -ge 1 ] || { echo "usage: mesh_pbr_viewer.sh <mesh-file> [out.png]" >&2; exit 2; }

GZ_O3DE_WS="${GZ_O3DE_WS:-/home/jrivero/code/gz/ws_o3de_rendering}"
GZ_O3DE_REPO="${GZ_O3DE_REPO:-/home/jrivero/code/gz/gz-rendering}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O3DE_LIBS="$GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile"

[ -f "$GZ_O3DE_WS/install/setup.bash" ] || {
  echo "ERROR: no colcon install at '$GZ_O3DE_WS/install'" >&2; exit 1; }
[ -d "$O3DE_LIBS" ] || {
  echo "ERROR: no vendored O3DE runtime libs at '$O3DE_LIBS'" >&2; exit 1; }
[ -x "$SCRIPT_DIR/build/mesh_pbr_viewer" ] || {
  echo "ERROR: example not built -- see README.md" >&2; exit 1; }

# colcon's setup.bash references unbound vars; relax nounset for the source.
set +u
# shellcheck disable=SC1091
source "$GZ_O3DE_WS/install/setup.bash"
set -u
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$O3DE_LIBS"

# Metals/speculars need IBL to read convincingly (M11-C).
export GZ_O3DE_DEMO_IBL=1

MESH="$1"
OUT="${2:-/tmp/mesh_pbr_viewer.png}"

[ -f "$MESH" ] || { echo "ERROR: mesh file not found: $MESH" >&2; exit 1; }

exec "$SCRIPT_DIR/build/mesh_pbr_viewer" "$MESH" "$OUT"
```

Then `chmod +x mesh_pbr_viewer.sh`.

- [ ] **Step 4: Write `README.md`**

```markdown
# mesh_pbr_viewer (M13)

Loads any mesh file gz-common can parse (GLB, .dae, .obj, ...) through the
public gz-rendering API on the o3de backend and renders it headless to a
PNG — **without calling SetMaterial**, so what you see is the material the
mesh FILE carries, auto-applied by the backend (M13): diffuse +
metallic/roughness factors and the albedo / normal / metalness / roughness
texture maps (GLB embedded textures and .dae sidecar texture files both
work).

The camera auto-frames from the mesh bounds; a ground box and key/fill
point lights (scaled to the model extent) are added with explicit gz
materials — those exercise the precedence rule (explicit gz material wins
over any file material).

## Build

    cd o3de/examples/mesh_pbr_viewer
    mkdir -p build && cd build
    source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash
    cmake .. && make

## Run

    ./mesh_pbr_viewer.sh <mesh-file> [out.png]

Example (jetty warehouse forklift, GLB with embedded PBR texture set):

    ./mesh_pbr_viewer.sh \
      /home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb \
      /tmp/forklift.png

Look for the backend telemetry line confirming the file material applied:

    [gz-o3de] M13 file-material id=... albedo=mem normal=mem metal=mem rough=mem factors m=... r=...

(`mem` = in-memory/embedded texture, `path` = sidecar file, `none` = map not
present, `fail` = decode/upload failed.)
```

- [ ] **Step 5: Build and run the example**

```bash
cd /home/jrivero/code/gz/gz-rendering/o3de/examples/mesh_pbr_viewer
mkdir -p build && cd build
set +u; source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash; set -u
cmake .. && make
cd ..
./mesh_pbr_viewer.sh \
  /home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb \
  /tmp/m13_forklift.png
```

Expected: exit 0, `[mesh_pbr_viewer] loaded: ... 15777 verts ...`, the M13 telemetry line with `albedo=mem`, and `[mesh_pbr_viewer] wrote /tmp/m13_forklift.png`.

- [ ] **Step 6: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/examples/mesh_pbr_viewer/
git ci -m "o3de: M13 mesh_pbr_viewer example

Promote the GLB smoke test into a real example: load any mesh file through
the public gz API with NO SetMaterial call, auto-frame the camera from the
bounds, render headless via CPU readback and write a PNG -- the driver for
the M13 mesh-file material auto-apply.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

(Make sure `build/` and any stray `imgui.ini` are NOT added — check `git status` first; the repo's existing example dirs keep `build/` untracked.)

---

### Task 5: Verification — textured Forklift, .dae path form, regressions, screenshots

**Files:**
- Create: `o3de/docs/screenshots/m13-forklift-textured.png`
- Create: `o3de/docs/screenshots/m13-forklift-before-grey.png` (copy of `/tmp/glb_smoke_forklift.png`)

- [ ] **Step 1: Forklift GLB (in-memory/embedded textures)**

```bash
cd /home/jrivero/code/gz/gz-rendering/o3de/examples/mesh_pbr_viewer
./mesh_pbr_viewer.sh \
  /home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb \
  /tmp/m13_forklift.png 2>&1 | tee /tmp/m13_forklift.log | grep -E "M13|loaded|wrote"
```

Check, in order:
1. Telemetry: `M13 file-material id=... albedo=mem ... factors m=... r=...` (the four map slots should mostly read `mem` for this GLB; `none` for a slot the asset genuinely lacks is fine, `fail` is NOT).
2. Visual (Read the PNG): the forklift shows its real paint/texture colors (yellow-ish body panels, dark wheels/mast) — clearly NOT the uniform grey of the before-image `/tmp/glb_smoke_forklift.png`.
3. Normal-map sanity: surface detail visible, not washed-out flat shading (if it looks chalky/flat, suspect an sRGB-vs-linear mixup in the normal upload — the `_srgb=false` argument).

- [ ] **Step 2: cordless_drill .dae (path-based sidecar texture)**

```bash
./mesh_pbr_viewer.sh \
  /home/jrivero/code/gz/jetty_demo/jetty_demo/models/cordless_drill_default_inertia/meshes/cordless_drill.dae \
  /tmp/m13_drill.png 2>&1 | grep -E "M13|texture file|loaded|wrote"
```

Check: telemetry shows `albedo=path` (or `albedo=mem` if gz-common pre-decodes — either proves the chain; `path` additionally proves `FileTexture`), a `texture file .../cordless_drill.png (srgb=1) -> READY` line if the path form was used, and the drill renders with its texture in the PNG.

- [ ] **Step 3: Regression — pbr_materials**

```bash
/home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials/pbr_materials.sh /tmp/m13_final_regression.png 2>&1 | grep -E "M13|wrote"
```

Expected: NO M13 telemetry; PNG visually matches `o3de/docs/screenshots/m12-pbr-materials-gz-api.png` (explicit gz materials untouched).

- [ ] **Step 4: Regression — live demo**

```bash
timeout 60 /home/jrivero/code/gz/gz-rendering/o3de/examples/native_vulkan_live/live_demo.sh > /tmp/m13_demo.log 2>&1 || true
grep -cE "M13 file-material" /tmp/m13_demo.log
```

Expected: `0` — the demo's primitive/runtime meshes carry no file material; demo renders as before (if a window-scoped capture is easy to take on :1, eyeball it; do NOT capture the root window, do NOT use Xvfb).

- [ ] **Step 5: Save the screenshots**

```bash
cp /tmp/m13_forklift.png \
   /home/jrivero/code/gz/gz-rendering/o3de/docs/screenshots/m13-forklift-textured.png
cp /tmp/glb_smoke_forklift.png \
   /home/jrivero/code/gz/gz-rendering/o3de/docs/screenshots/m13-forklift-before-grey.png
```

- [ ] **Step 6: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/docs/screenshots/m13-forklift-textured.png \
        o3de/docs/screenshots/m13-forklift-before-grey.png
git ci -m "o3de(docs): M13 verification screenshots -- textured forklift vs grey before

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Handoff docs + memory updates

**Files:**
- Modify: `o3de/docs/HANDOFF_FOR_NEXT_AGENT.md`
- Modify: `/home/jrivero/.claude/memory/project_o3de_rendering_roadmap.md` (NOT committed to the repo)

- [ ] **Step 1: Update the handoff doc**

In `o3de/docs/HANDOFF_FOR_NEXT_AGENT.md`:
1. Add an M13 row to the milestone table, mirroring the M12 row's style: scope (mesh-file material auto-apply: factors + albedo/normal/metalness/roughness maps, GLB embedded + .dae path forms, explicit-gz-material-wins precedence), key commits, verification artifacts (`m13-forklift-textured.png` vs `m13-forklift-before-grey.png`, `mesh_pbr_viewer` example).
2. Refresh the TL;DR so it mentions real assets now render with their file materials.
3. Update "Suggested next moves" (natural candidates: per-submesh material slots, emissive maps, jetty_demo full-scene showcase).
4. Mention the new example dir `o3de/examples/mesh_pbr_viewer/` wherever the doc lists examples.

- [ ] **Step 2: Update the roadmap memory**

In `/home/jrivero/.claude/memory/project_o3de_rendering_roadmap.md`, extend the status with: M13 landed — mesh-file materials auto-apply (GLB embedded PBR sets + .dae sidecar textures; material[0] whole-mesh; explicit gz material wins; sRGB albedo / linear data maps; `mesh_pbr_viewer` example; verified on jetty Forklift GLB + cordless_drill.dae).

- [ ] **Step 3: Commit the handoff (repo files only)**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/docs/HANDOFF_FOR_NEXT_AGENT.md
git ci -m "o3de(docs): handoff -- M13 real-asset PBR (mesh-file materials auto-apply)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review notes (done at plan-writing time)

- **Spec coverage:** extraction struct/selection/warning → Task 1; upload helper + sRGB split + caches → Task 2; application/precedence/telemetry + error handling (skip-on-fail, non-TANGENT skip, factors-still-apply) → Tasks 1+3; example → Task 4; all four verification points of the spec → Task 5; docs/memory contract → Task 6.
- **Spec deviation (documented):** the spec's cache key for in-memory images said "data pointer + name"; the plan keys on the `common::Image` object address + srgb flag only (the shared_ptr in `meshFileMaterials` pins the address; a name adds nothing for identity). Also `FileBaseColorImage` is renamed `FileTexture` rather than kept as a wrapper — single call site, no API surface.
- **Type consistency:** `MeshFileMaterialCpu` field names (`albedoImg/albedoPath/...`) match between Task 1 (definition), Task 3 (use); helper names `StreamingImageFromCommonImage`/`FileTexture`/`MemTexture` match between Task 2 (definition) and Task 3 (use).
- **`normal.factor`** intentionally untouched (spec: stays at Atom default).
