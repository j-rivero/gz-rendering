# O3DE / Atom render engine (experimental PoC)

This directory contains an **experimental, proof-of-concept** gz-rendering backend
that renders through [O3DE](https://o3de.org)'s **Atom** renderer, alongside the
existing `ogre` and `ogre2` engines.

> **Status: PoC foundation only.** It renders primitive shapes (box, sphere,
> cylinder) via Atom's AuxGeom and makes them visible in gz-gui's `MinimalScene`
> through the CPU-readback fallback. It is **not** production-ready, is gated
> `OFF` by default (`BUILD_O3DE`), and must never affect a normal gz-rendering
> build. Performance, full materials/meshes, lights and shadows are out of scope
> for this phase.

## How it works

* The engine reports `GraphicsAPI() == VULKAN`, so gz-gui's `MinimalScene` uses
  its built-in CPU-readback path (`camera->Copy(image)`) — no GL/Vulkan texture
  sharing is needed.
* `O3deRenderTarget::Copy()` gathers the camera pose/projection and the scene's
  primitive visuals (type + world pose + scale + diffuse colour) and forwards
  them to a single Atom-owning translation unit, `O3deBackend.cc`.
* **All O3DE/Atom work runs on one dedicated thread** owned by `O3deBackend`
  (bootstrap *and* every render tick). This is mandatory: Atom's `AssetManager`
  only self-pumps a synchronous (critical-shader) load when it runs on the
  thread that created the application. Driving ticks from gz-gui's render thread
  instead deadlocks on a blocking shader load during a pass-tree rebuild.
* The Atom runtime is brought up once and **never torn down** (tearing down a
  live `RPISystem` crashes in Vulkan teardown); the OS reclaims it at exit.

Only `O3deBackend.cc` includes Atom/AzCore headers; it is compiled with O3DE's
own compile model (clang, `-fno-exceptions`, C++20, O3DE defines) via
per-source properties in `src/CMakeLists.txt`. The rest of the component is
ordinary gz-rendering C++ that talks to it through the plain-C++ `O3deBackend`
interface.

## Reference environment (exact state used to produce this PoC)

This backend was developed and verified against the specific O3DE checkout and
toolchain below. O3DE's `development` branch and Atom move quickly; to fully
reproduce the run, pin to these versions.

| Component | Value |
|-----------|-------|
| OS | Ubuntu 24.04.4 LTS, kernel 6.17 |
| GPU / driver | NVIDIA GeForce RTX 4060 Ti, driver 580.159.03 |
| Vulkan | API 1.4.312 (NVIDIA ICD) |
| Compiler | clang / clang++ **18.1.3** |
| CMake | 3.28.3 |
| Ninja | from a local venv (`uv venv … && uv pip install ninja`), on `PATH` |
| **O3DE engine** | repo `git@github.com:o3de/o3de.git`, branch `development`, commit **`5bdb8cc3111d5064438780503f09e78ccc850738`**, engine version `4.2.0` |
| O3DE 3rd-party (`LY_3RDPARTY_PATH=~/o3de-packages`) | 62 packages; key ones: `RapidJSON-1.1.0-rev1`, `RapidXML-1.13-rev1`, `cityhash-1.1`, `zlib-1.2.11-rev5`, `lz4-1.9.4-rev2`, `Lua-5.4.4-rev1`, `qt-5.15.2-rev9` |
| O3DE build | `vendor/o3de/build/linux` (`profile` config, Ninja Multi-Config, clang-18) |

### O3DE engine checkout

```bash
git clone git@github.com:o3de/o3de.git vendor/o3de
git -C vendor/o3de checkout 5bdb8cc3111d5064438780503f09e78ccc850738
```

The 3rd-party packages are fetched automatically by O3DE's build into
`LY_3RDPARTY_PATH` (`~/o3de-packages`) from its CDN; pinning the engine commit
pins the package revisions it requests.

### Cooked project (`GzAtomPoc`)

Atom loads cooked assets (pipelines, AuxGeom shaders) at runtime from a project
cache. The PoC uses a minimal project named **`GzAtomPoc`** (created from
O3DE's `MinimalProject` template):

| Item | Value |
|------|-------|
| Project name | `GzAtomPoc` (default `GZ_O3DE_PROJECT_NAME`) |
| Enabled gems | `GzAtomPoc`, `Atom`, `CameraFramework`, `ImGui`, `ScriptAutomation` |
| `engine_version` | `4.2.0` |
| Cooked cache | `~/o3de-gzpoc/Cache/linux` (~885 MB, `AssetProcessorBatch --platforms=linux`) |

> The backend only needs the cooked **cache** + the Atom **gem set** this
> project pulls in; it does not run any project game code.

## Prerequisites

1. **clang-18.** O3DE is built with clang and its Atom RHI headers use
   clang-only template/anonymous-aggregate patterns that GCC rejects, so the
   whole `BUILD_O3DE` configuration must use clang.

2. **A prebuilt O3DE engine tree.** The component consumes O3DE as a *prebuilt*
   tree (it does **not** `add_subdirectory` it). The CMake expects, relative to
   the gz-rendering source root:

   | Path | Contents |
   |------|----------|
   | `vendor/o3de/`                | the O3DE engine source clone (`O3DE_ROOT`) |
   | `vendor/o3de/build/linux/`    | its CMake build dir (`profile` config) |
   | `~/o3de-packages/packages/`   | the O3DE 3rd-party package cache (`LY_3RDPARTY_PATH`) |

   This vendored O3DE tree (~16 GB) is intentionally **git-ignored** and is *not*
   part of this repository.

3. **A cooked O3DE project** providing the Atom gem set + the asset cache that
   Atom loads at runtime (pipelines, AuxGeom shaders, etc.). The PoC uses a
   minimal project named `GzAtomPoc`.

4. **System packages** for O3DE on Linux (X11/XCB):
   `libxcb-xkb-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-keysyms1-dev`
   `libxcb-image0-dev libxkbcommon-x11-dev`.

### Building the O3DE prerequisite (one-time)

```bash
# In the O3DE clone (e.g. vendor/o3de), with ninja on PATH:
./scripts/o3de.sh register --this-engine
cmake -B build/linux -S . -G "Ninja Multi-Config" \
  -DLY_3RDPARTY_PATH=$HOME/o3de-packages \
  -DLY_DISABLE_TEST_MODULES=ON -DLY_UNITY_BUILD=ON \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
# Build the framework + Atom libraries (profile config; -j tuned for RAM):
cmake --build build/linux --config profile -j5 --target \
  AzCore AzFramework AzGameFramework Atom_RHI.Public \
  Atom_RHI_Vulkan.Private Atom_RPI.Public Atom_Feature_Common \
  Atom_Bootstrap AssetProcessorBatch

# Create + cook the minimal project (provides gems + asset cache):
#   reconfigure with -DLY_PROJECTS=<project-path>, build <Project>.GameLauncher,
#   then cook:  AssetProcessorBatch --project-path=<project-path> --platforms=linux
```

## Building the gz-rendering O3DE backend

```bash
cmake -S . -B build_o3de \
  -DBUILD_O3DE=ON \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build_o3de --target gz-rendering-o3de -j5
```

`BUILD_O3DE` defaults to `OFF`; without it the backend is not built or required.

## Running

The plugin loads the O3DE runtime in-process. Point the dynamic loader at both
the freshly built plugin and the O3DE binaries, and tell the backend where the
engine/project live (the defaults assume the author's layout — override them):

```bash
export LD_LIBRARY_PATH=$PWD/build_o3de/lib:$PWD/vendor/o3de/build/linux/bin/profile
export GZ_RENDERING_PLUGIN_PATH=$PWD/build_o3de/lib

# Backend paths (override the hard-coded PoC defaults as needed):
export GZ_O3DE_ENGINE_PATH=$PWD/vendor/o3de          # O3DE engine root
export GZ_O3DE_PROJECT_PATH=$HOME/o3de-gzpoc         # cooked project root
export GZ_O3DE_PROJECT_NAME=GzAtomPoc
# export GZ_O3DE_BIN_PATH=...   # defaults to $GZ_O3DE_ENGINE_PATH/build/linux/bin/profile

# O3DE/Atom opens a window on Linux; a display is required.
export DISPLAY=:1

# Optional helpers:
#   GZ_O3DE_DEMO_SHAPES=1  inject a red box / green sphere / blue cylinder when
#                          the scene has no primitives (e.g. an empty MinimalScene)
#   GZ_O3DE_DUMP_FRAME=1   dump the first rendered frame to /tmp/gz_gui_frame.ppm

GZ_O3DE_DEMO_SHAPES=1 gz gui -c examples/config/scene3d.config   # <engine>o3de</engine>
```

`gz sim` / `gz gui` select this engine when the rendering config requests
`<engine>o3de</engine>` (or via `--render-engine o3de`).

## Known limitations

* **Performance:** rendering goes through gz-gui's CPU-readback fallback, which
  gz-gui itself warns is slow. Acceptable for the PoC; a zero-copy Vulkan↔GL
  path is future work.
* **Primitives only:** box / sphere / cylinder / cone via AuxGeom with flat
  diffuse colour. No meshes, textures, PBR materials, lights or shadows yet.
* **Teardown:** O3DE's static teardown of a live `RPISystem` can SIGSEGV at
  process exit (after rendering has finished). The runtime is deliberately left
  running for the life of the process; the render path is unaffected.
* **Hard-coded paths:** build-time O3DE paths (`vendor/o3de`, `~/o3de-packages`)
  are fixed in `src/CMakeLists.txt`; runtime paths are overridable via the
  `GZ_O3DE_*` environment variables above.
