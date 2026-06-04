# M12 — Real gz Materials on Runtime Meshes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Feed the proven Atom PBR pipeline (diffuse, metallic, roughness, albedo texture file) from real gz-rendering materials created through the public API, verified by a new standalone example.

**Architecture:** Extend the existing per-frame `GatherFrame` snapshot in `O3deRenderTarget.cc` to copy `Metalness()`/`Roughness()`/`Texture()` from the gz material into `O3deMeshData` (whose fields the backend already consumes, unchanged, since M11). Add `o3de/examples/pbr_materials/` — a headless C++ example that builds the scene 100% through the public gz API and captures a PNG via the M0/M2 CPU-readback path.

**Tech Stack:** C++17, gz-rendering public API, gz-common Image, CMake (standalone example pattern like `examples/mesh_viewer`), bash launcher.

**Spec:** `o3de/docs/specs/2026-06-04-m12-real-gz-materials-design.md`

**Testing note:** This PoC has no unit-test harness for the o3de plugin (every code path requires the bootstrapped Atom runtime + GPU). Following the project's established convention (M0–M11), verification is integration-level: the new example IS the test — it exercises the full public-API chain and produces a PNG checked against the M11 reference screenshots. Do NOT use Xvfb (lavapipe confound); the readback path needs no window at all.

**Critical environment facts (from project memory):**
- Rebuild with `cmake --build . --target gz-rendering-o3de -j5` in `build_o3de` — NEVER `--target o3de` (silent no-op).
- After rebuilding, copy the `.so` over BOTH workspace paths or you run a stale plugin.
- Commits: `git ci` (alias adds `--signoff`), email `jrivero@honurobotics.com`, trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Never `rm -rf`; move to `/tmp/` instead.
- Do NOT `git push` without explicit user confirmation.

---

### Task 1: Wire PBR properties into the GatherFrame snapshot

**Files:**
- Modify: `o3de/src/O3deRenderTarget.cc` (mesh branch of `GatherFrame`, ~line 179)
- Modify: `o3de/include/gz/rendering/o3de/O3deMaterial.hh` (class comment only)

- [ ] **Step 1: Extend the mesh-material read**

In `o3de/src/O3deRenderTarget.cc`, find this block (inside the
`if (auto o3deMesh = std::dynamic_pointer_cast<O3deMesh>(geom))` branch):

```cpp
          if (MaterialPtr mat = o3deMesh->Material())
          {
            const math::Color c = mat->Diffuse();
            md.color[0] = c.R();
            md.color[1] = c.G();
            md.color[2] = c.B();
            md.color[3] = c.A();
          }
```

Replace with:

```cpp
          if (MaterialPtr mat = o3deMesh->Material())
          {
            const math::Color c = mat->Diffuse();
            md.color[0] = c.R();
            md.color[1] = c.G();
            md.color[2] = c.B();
            md.color[3] = c.A();
            // M12: real gz material -> Atom StandardPBR. Set unconditionally
            // when a material is attached: gz defaults are legitimate values;
            // the -1 "leave Atom default" sentinels remain only for
            // material-less meshes. A non-empty texture path is decoded +
            // cached backend-side (FileBaseColorImage, M11-D); empty means
            // no albedo map, exactly as before.
            md.metallic = mat->Metalness();
            md.roughness = mat->Roughness();
            md.texturePath = mat->Texture();
          }
```

- [ ] **Step 2: Update the O3deMaterial class comment**

In `o3de/include/gz/rendering/o3de/O3deMaterial.hh`, replace:

```cpp
    /// For the M0 stub the color/PBR state is held by BaseMaterial; no
    /// Atom material asset is created yet. A later milestone will map the
    /// diffuse/ambient color onto AuxGeom draws.
```

with:

```cpp
    /// The color/PBR state is held by BaseMaterial; no Atom-side object
    /// exists per gz material. Properties cross to the renderer via the
    /// per-frame snapshot: O3deRenderTarget::GatherFrame reads Diffuse /
    /// Metalness / Roughness / Texture into O3deMeshData (M12) and the
    /// backend applies them to the per-mesh StandardPBR material instance
    /// (M11). Mesh geometries only; AuxGeom primitives use the flat
    /// diffuse colour.
```

- [ ] **Step 3: Build the plugin**

```bash
cd /home/jrivero/code/gz/gz-rendering/build_o3de
cmake --build . --target gz-rendering-o3de -j5
```

Expected: compiles with no errors, ends with
`[100%] Built target gz-rendering-o3de` (or similar link line).

- [ ] **Step 4: Verify the new code is in the .so**

```bash
strings /home/jrivero/code/gz/gz-rendering/build_o3de/lib/libgz-rendering-o3de.so.11.0.0~pre1 | grep -c "M12: real gz material" || true
```

Expected: `0` is FINE here (comments aren't strings) — instead confirm the
object recompiled:

```bash
ls -la --time-style=full-iso /home/jrivero/code/gz/gz-rendering/build_o3de/lib/libgz-rendering-o3de.so.11.0.0~pre1
```

Expected: timestamp within the last minute.

- [ ] **Step 5: Commit**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/src/O3deRenderTarget.cc o3de/include/gz/rendering/o3de/O3deMaterial.hh
git ci -m "o3de: M12 — wire real gz material PBR properties into the frame snapshot

GatherFrame's mesh branch now copies Metalness()/Roughness()/Texture()
from the attached gz material into O3deMeshData, alongside the Diffuse()
it already copied. The backend consumes these fields unchanged (M11
proved them end-to-end via demo injection); this makes a real gz
material created through the public API drive them instead.

Set unconditionally when a material is attached: gz defaults are
legitimate values; the -1 sentinels remain only for material-less
meshes. Verified by the new pbr_materials example (next commit).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: The pbr_materials standalone example

**Files:**
- Create: `o3de/examples/pbr_materials/Main.cc`
- Create: `o3de/examples/pbr_materials/CMakeLists.txt`
- Create: `o3de/examples/pbr_materials/pbr_materials.sh`
- Create: `o3de/examples/pbr_materials/README.md`

- [ ] **Step 1: Write `Main.cc`**

```cpp
/*
 * Copyright (C) 2026 Open Source Robotics Foundation
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

// M12: real gz materials -> Atom PBR, 100% through the public gz-rendering
// API. No demo injection (GZ_O3DE_DEMO_SHAPES stays unset), no Qt, no
// interop: the scene below is built with CreateMesh/CreateMaterial/
// CreatePointLight and rendered through the CPU-readback path
// (Camera::Capture -> O3deRenderTarget::Copy), then saved as a PNG.
//
// Usage: pbr_materials [albedo.png] [output.png]
// (run via pbr_materials.sh, which sets up the plugin + Atom runtime env)

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <gz/common/Image.hh>

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

namespace
{

/// \brief One unit_sphere visual with the given material at _pos.
VisualPtr AddSphere(ScenePtr _scene, VisualPtr _root,
    const std::string &_name, const math::Vector3d &_pos, MaterialPtr _mat)
{
  MeshDescriptor descriptor("unit_sphere");
  MeshPtr mesh = _scene->CreateMesh(descriptor);
  if (!mesh)
  {
    std::fprintf(stderr, "[pbr_materials] CreateMesh(unit_sphere) FAILED\n");
    return nullptr;
  }
  mesh->SetMaterial(_mat);
  VisualPtr v = _scene->CreateVisual(_name);
  v->AddGeometry(mesh);
  v->SetLocalPosition(_pos);
  _root->AddChild(v);
  return v;
}

}  // namespace

int main(int _argc, char **_argv)
{
  const std::string texPath = _argc > 1 ? _argv[1] : "";
  const std::string outPath =
      _argc > 2 ? _argv[2] : "/tmp/m12_pbr_materials.png";

  RenderEngine *engine = rendering::engine("o3de");
  if (!engine)
  {
    std::fprintf(stderr, "[pbr_materials] engine 'o3de' not found -- check "
        "GZ_RENDERING_PLUGIN_PATH (run via pbr_materials.sh)\n");
    return 1;
  }

  ScenePtr scene = engine->CreateScene("scene");
  scene->SetAmbientLight(0.3, 0.3, 0.3);
  VisualPtr root = scene->RootVisual();

  // Ground: a flattened unit_box with a rough dielectric grey.
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.40, 0.42, 0.45);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.9f);
    MeshDescriptor desc("unit_box");
    MeshPtr mesh = scene->CreateMesh(desc);
    if (!mesh)
    {
      std::fprintf(stderr, "[pbr_materials] CreateMesh(unit_box) FAILED\n");
      return 1;
    }
    mesh->SetMaterial(mat);
    VisualPtr ground = scene->CreateVisual("ground");
    ground->AddGeometry(mesh);
    ground->SetLocalScale(12.0, 12.0, 0.5);
    ground->SetLocalPosition(0.0, 0.0, -0.25);  // top face at z=0
    root->AddChild(ground);
  }

  // Bottom row: 5-sphere metallic roughness sweep 0.05 -> 0.95 (mirrors the
  // proven M11-A demo scene for direct visual comparison).
  for (int i = 0; i < 5; ++i)
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(0.95, 0.95, 0.95);
    mat->SetMetalness(1.0f);
    mat->SetRoughness(0.05f + 0.225f * static_cast<float>(i));
    AddSphere(scene, root, "sweep_" + std::to_string(i),
        math::Vector3d(0.0, -2.0 + i, 0.5), mat);
  }

  // Top row, left: gold (warm metal -- needs IBL to read as metal).
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(1.0, 0.77, 0.34);
    mat->SetMetalness(1.0f);
    mat->SetRoughness(0.25f);
    AddSphere(scene, root, "gold", math::Vector3d(0.0, -0.75, 1.6), mat);
  }

  // Top row, right: real albedo texture FILE through Material::SetTexture --
  // the exact path a gz-sim material's base-color map takes (M11-D backend).
  {
    MaterialPtr mat = scene->CreateMaterial();
    mat->SetDiffuse(1.0, 1.0, 1.0);
    mat->SetMetalness(0.0f);
    mat->SetRoughness(0.6f);
    if (!texPath.empty())
      mat->SetTexture(texPath);
    else
      std::fprintf(stderr, "[pbr_materials] no albedo path given -- "
          "textured sphere will be plain white\n");
    AddSphere(scene, root, "textured", math::Vector3d(0.0, 0.75, 1.6), mat);
  }

  // Lights: key + fill point pair (same values as the backend's PBR_ONLY
  // demo lighting, so captures compare 1:1 against the M11 references).
  {
    PointLightPtr key = scene->CreatePointLight();
    key->SetLocalPosition(-1.5, 1.5, 3.0);
    key->SetDiffuseColor(1.0, 0.97, 0.92);
    key->SetIntensity(220.0);
    key->SetAttenuationRange(18.0);
    root->AddChild(key);

    PointLightPtr fill = scene->CreatePointLight();
    fill->SetLocalPosition(-2.0, -1.5, 1.2);
    fill->SetDiffuseColor(0.6, 0.7, 0.95);
    fill->SetIntensity(70.0);
    fill->SetAttenuationRange(16.0);
    root->AddChild(fill);
  }

  CameraPtr camera = scene->CreateCamera("camera");
  camera->SetLocalPosition(-5.5, 0.0, 1.8);
  camera->SetLocalRotation(0.0, 0.15, 0.0);  // pitch down toward the rows
  camera->SetImageWidth(1280);
  camera->SetImageHeight(720);
  camera->SetAspectRatio(1280.0 / 720.0);
  camera->SetHFOV(1.047);
  root->AddChild(camera);

  // Capture. The first frames ride out Atom's boot/warmup (~3 s); each
  // Capture() drives a full synchronous offscreen frame + CPU readback.
  Image image = camera->CreateImage();
  for (int frame = 0; frame < 30; ++frame)
  {
    camera->Capture(image);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  common::Image out;
  out.SetFromData(image.Data<unsigned char>(),
      camera->ImageWidth(), camera->ImageHeight(), common::Image::RGB_INT8);
  out.SavePNG(outPath);
  std::printf("[pbr_materials] wrote %s\n", outPath.c_str());

  // No teardown: the o3de backend is a process-wide singleton by design
  // (see O3deBackend docs); process exit is the supported shutdown.
  return 0;
}
```

NOTE for the implementer: the camera's default image format is `PF_R8G8B8`,
which `O3deRenderTarget::Copy()` supports (it renders RGBA internally and
drops alpha). If `image.Format()` turns out to be 4-channel on this branch,
switch the `SetFromData` format to `common::Image::RGBA_INT8`.

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(gz-rendering-pbr-materials)

find_package(gz-rendering REQUIRED)

add_executable(pbr_materials Main.cc)

target_link_libraries(pbr_materials
  ${GZ-RENDERING_LIBRARIES}
)
```

- [ ] **Step 3: Write `pbr_materials.sh`**

```bash
#!/usr/bin/env bash
#
# M12: real gz materials -> Atom PBR through the public gz-rendering API.
#
# Renders a headless offscreen scene via the CPU-readback path (no Qt, no
# zero-copy interop) and writes a PNG. The scene -- a metallic roughness
# sweep, a gold sphere, a file-textured sphere, ground, two point lights --
# is built 100% with public gz API calls; GZ_O3DE_DEMO_SHAPES stays unset
# so the backend's demo injection is off.
#
# Usage: pbr_materials.sh [output.png]      (default /tmp/m12_pbr_materials.png)
#
# Env overrides (same as live_demo.sh):
#   GZ_O3DE_WS    colcon workspace whose install/ holds gz-rendering
#   GZ_O3DE_REPO  gz-rendering source checkout (for vendor/o3de runtime libs)

set -euo pipefail

GZ_O3DE_WS="${GZ_O3DE_WS:-/home/jrivero/code/gz/ws_o3de_rendering}"
GZ_O3DE_REPO="${GZ_O3DE_REPO:-/home/jrivero/code/gz/gz-rendering}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O3DE_LIBS="$GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile"

[ -f "$GZ_O3DE_WS/install/setup.bash" ] || {
  echo "ERROR: no colcon install at '$GZ_O3DE_WS/install'" >&2; exit 1; }
[ -d "$O3DE_LIBS" ] || {
  echo "ERROR: no vendored O3DE runtime libs at '$O3DE_LIBS'" >&2; exit 1; }
[ -x "$SCRIPT_DIR/build/pbr_materials" ] || {
  echo "ERROR: example not built -- see README.md" >&2; exit 1; }

# colcon's setup.bash references unbound vars; relax nounset for the source.
set +u
# shellcheck disable=SC1091
source "$GZ_O3DE_WS/install/setup.bash"
set -u
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$O3DE_LIBS"

# Metals need IBL to read as metal (M11-C). Readback path; no interop vars.
export GZ_O3DE_DEMO_IBL=1

TEX="$SCRIPT_DIR/../native_vulkan_live/assets/gz_albedo_demo.png"
OUT="${1:-/tmp/m12_pbr_materials.png}"

exec "$SCRIPT_DIR/build/pbr_materials" "$TEX" "$OUT"
```

- [ ] **Step 4: Write `README.md`**

```markdown
# pbr_materials — real gz materials on the o3de backend (M12)

Builds a scene 100% through the public gz-rendering API (no demo
injection): a metallic roughness sweep (0.05→0.95), a gold sphere, a
sphere with a real albedo texture file (`Material::SetTexture`), a
ground box and two point lights. Renders headless via the CPU-readback
path and writes a PNG.

## Build

```bash
source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash
cmake -S . -B build
cmake --build build -j5
```

## Run

```bash
./pbr_materials.sh                 # writes /tmp/m12_pbr_materials.png
./pbr_materials.sh out.png         # custom output path
```

Expect: bottom row of 5 chrome-like spheres going mirror→dull left to
right, a gold sphere and a "gz-rendering"-textured sphere floating above,
all PBR-lit on a grey ground. Compare with
`../../docs/screenshots/m12-pbr-materials-gz-api.png`.
```

- [ ] **Step 5: Make the launcher executable and build the example**

```bash
chmod +x /home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials/pbr_materials.sh
cd /home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials
set +u; source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash; set -u
cmake -S . -B build
cmake --build build -j5
```

Expected: configure finds `gz-rendering`, build ends with
`[100%] Built target pbr_materials`. (If `find_package(gz-rendering)`
fails, retry with `cmake -S . -B build -DCMAKE_PREFIX_PATH=/home/jrivero/code/gz/ws_o3de_rendering/install`.)

- [ ] **Step 6: Commit (sources only — NOT the build/ directory)**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/examples/pbr_materials/Main.cc \
        o3de/examples/pbr_materials/CMakeLists.txt \
        o3de/examples/pbr_materials/pbr_materials.sh \
        o3de/examples/pbr_materials/README.md
git ci -m "o3de: M12 — pbr_materials example, scene 100% via the public gz API

Standalone headless example proving the real gz-material chain end to
end: CreateMesh(unit_sphere/unit_box) + CreateMaterial with SetDiffuse/
SetMetalness/SetRoughness/SetTexture + CreatePointLight, rendered
through the CPU-readback path (Camera::Capture -> Copy) with no demo
injection, no Qt and no interop. Mirrors the proven M11 demo scene
(roughness sweep, gold, file-textured sphere, key+fill lights) so the
capture compares 1:1 against the M11 reference screenshots.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Run, verify, capture reference screenshot

**Files:**
- Create: `o3de/docs/screenshots/m12-pbr-materials-gz-api.png` (the verified capture)

- [ ] **Step 1: Install the rebuilt plugin over BOTH workspace copies**

```bash
SRC=/home/jrivero/code/gz/gz-rendering/build_o3de/lib/libgz-rendering-o3de.so.11.0.0~pre1
WS=/home/jrivero/code/gz/ws_o3de_rendering/install/lib
cp -f "$SRC" "$WS/libgz-rendering-o3de.so.11.0.0~pre1"
cp -f "$SRC" "$WS/gz-rendering/engine-plugins/libgz-rendering-o3de.so.11.0.0~pre1"
```

- [ ] **Step 2: Run the example**

```bash
cd /home/jrivero/code/gz/gz-rendering/o3de/examples/pbr_materials
./pbr_materials.sh /tmp/m12_pbr_materials.png 2>&1 | tee /tmp/o3de_debug_m12_run.log
```

Expected: Atom boot logs (~3 s), per-mesh `RegisterMesh` telemetry, a
texture decode line for `gz_albedo_demo.png`, NO demo-injection lines
(no `MaybeInjectDemoShapes` output), and finally
`[pbr_materials] wrote /tmp/m12_pbr_materials.png`.

- [ ] **Step 3: Verify the capture is a real render (not grey/black)**

```bash
identify /tmp/m12_pbr_materials.png
convert /tmp/m12_pbr_materials.png -format %k info:
```

Expected: `1280x720`, and a unique-color count in the thousands+ (a failed/
grey frame has ~1 color). Then visually inspect the PNG (Read tool):
- bottom row: 5 metal spheres, mirror-sharp -> dull, left to right
- gold sphere reads gold (warm reflective), not black
- textured sphere shows the wrapped "gz-rendering" albedo image
- grey ground, warm key + cool fill lighting

Compare against `o3de/docs/screenshots/m11-pbr-*.png` references. If
framing is off (spheres cropped), nudge the camera position/pitch in
Main.cc, rebuild the example only (`cmake --build build -j5`), and rerun —
the plugin does not need rebuilding for example-side tweaks.

- [ ] **Step 4: Failure triage (only if Step 3 fails)**

- All-grey/black PNG + `RenderFrame` failures in the log → check the
  plugin .so timestamps in BOTH `$WS` paths (stale-plugin gotcha).
- Spheres render but all look identical chrome → the wiring didn't take:
  confirm Task 1's edit is in the running .so (`ls -la` timestamps), and
  that `RegisterMesh` telemetry shows 7 meshes.
- Textured sphere plain white → check the texture decode log line; verify
  the path printed by the launcher exists.
- Engine fails to load → run via the launcher (env), never the bare binary.

- [ ] **Step 5: Save the reference screenshot and commit**

```bash
cp /tmp/m12_pbr_materials.png \
   /home/jrivero/code/gz/gz-rendering/o3de/docs/screenshots/m12-pbr-materials-gz-api.png
cd /home/jrivero/code/gz/gz-rendering
git add o3de/docs/screenshots/m12-pbr-materials-gz-api.png
git ci -m "o3de(docs): M12 reference capture — real gz materials via public API

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Documentation + memory updates (handoff contract)

**Files:**
- Modify: `o3de/docs/HANDOFF_FOR_NEXT_AGENT.md`
- Modify: `~/.claude/memory/project_o3de_rendering_roadmap.md` (NOT committed to the repo)

- [ ] **Step 1: Add the M12 milestone row**

In `o3de/docs/HANDOFF_FOR_NEXT_AGENT.md`, append to the milestones table
(after the M11 row):

```markdown
| M12 | **Real gz materials on runtime meshes** — the public-API chain | ✓ landed. `GatherFrame`'s mesh branch copies `Metalness()`/`Roughness()`/`Texture()` from the attached gz material into `O3deMeshData` (backend consumes them unchanged since M11). Proven by `o3de/examples/pbr_materials/` — a headless example building the scene 100% via the public gz API (CreateMesh + CreateMaterial + CreatePointLight, no demo injection) and capturing through the CPU-readback path (`m12-pbr-materials-gz-api.png`). Scope: mesh geometries only (primitives stay AuxGeom); proven property set only (no emissive/normal/transparency). Per-submesh materials still no-op. Spec: `o3de/docs/specs/2026-06-04-m12-real-gz-materials-design.md`. |
```

- [ ] **Step 2: Update the TL;DR + "Last updated" header**

In the same file, update the `Last updated:` line to reference the new
last commit, and add one sentence to the TL;DR status paragraph:

```markdown
**M12 landed**: real gz materials (diffuse/metalness/roughness/albedo
file) now flow from the public gz API to Atom on runtime meshes,
verified by the headless `pbr_materials` example — the first scene
content that is 100% gz-sourced (no demo injection).
```

Also update "Suggested next moves": the gz-material wiring item is done;
next candidates are the cleanup TODOs (redundant A2 ProjectedShadow,
cosmetic BOXFILL, inert emissive crutch), the Qt 6.8 upgrade, and
consuming a real gz-sim scene end-to-end.

- [ ] **Step 3: Update the roadmap memory entry**

Edit `~/.claude/memory/project_o3de_rendering_roadmap.md`: append to the
status that M12 landed (real gz materials via public API, pbr_materials
example, mesh-only scope), keeping the existing entry structure.

- [ ] **Step 4: Commit the docs**

```bash
cd /home/jrivero/code/gz/gz-rendering
git add o3de/docs/HANDOFF_FOR_NEXT_AGENT.md
git ci -m "o3de(docs): handoff — M12 real gz materials via the public API

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Verification summary (whole plan)

1. Plugin builds clean with the 3-line wiring (Task 1).
2. Example builds standalone against the workspace install (Task 2).
3. The PNG shows the M11-proven visuals sourced 100% from public gz API
   calls — roughness sweep gradient, gold metal, wrapped albedo texture
   (Task 3). This is the milestone's acceptance test.
4. Handoff + memory updated per the project's handoff contract (Task 4).
```
