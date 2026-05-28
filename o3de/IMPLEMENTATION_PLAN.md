# O3DE / Atom render-engine backend — implementation plan

This is the implementation plan for the experimental O3DE/Atom gz-rendering
backend in this directory. It records the goal, the design decisions, the
milestone breakdown, and the status of each milestone. See [README.md](README.md)
for build/run instructions and the reference environment.

## Goal & scope

Explore wrapping **O3DE / Atom** (Open 3D Engine's renderer) as a *third*
gz-rendering backend, alongside `ogre` and `ogre2`, so a future Gazebo could
render through it.

**Phase 1 goal (this work): a PoC foundation** — a clean, reusable component
that gets a few primitive shapes (box, sphere, cylinder) rendering and
**visible in gz-gui's `MinimalScene`**, proving feasibility and seeding a real
engine plugin. Performance, full materials/meshes, lights and shadows are out
of scope for Phase 1.

Decisions taken up front:
1. Scope = PoC foundation (minimal shapes, clean/reusable, not production).
2. O3DE is consumed as a **prebuilt vendored tree** (`vendor/o3de`, git-ignored),
   linked against — *not* `add_subdirectory`'d (O3DE uses Ninja Multi-Config +
   engine registration + a gem system that does not compose as a subdirectory).
3. First display path = **CPU readback** (decouple O3DE-Vulkan from Qt-GL).

## Key finding that shapes the approach

gz-gui's `MinimalScene` already has a CPU-readback fallback
(`EngineToQtInterface::NeedsFallback()`): when `engine->GraphicsAPI()` is not
`OPENGL`/`METAL`, it allocates a `PF_R8G8B8A8` image, calls **`camera->Copy(image)`**,
and uploads it to a Qt GL texture itself. So the PoC only needs to (a) report
`GraphicsAPI() == VULKAN` and (b) implement `O3deRenderTarget::Copy(Image&)`.
No GL/Vulkan texture sharing — that is deferred to a later performance phase.

## Component layout

`o3de/` is the gz component (the directory name must be `o3de` because gz-cmake
maps component name → directory). It mirrors the minimal subset of `ogre2/`:

```
o3de/
  README.md                     build/run guide + reference environment
  IMPLEMENTATION_PLAN.md        this file
  include/gz/rendering/o3de/     public headers (+ o3de.hh.in, CMake chain)
  src/                           sources + CMakeLists.txt
    O3deBackend.{hh,cc}          the ONLY Atom-owning translation unit
```

The vendored upstream O3DE clone lives at `vendor/o3de/` (git-ignored).

## Class mapping (gz Base ↔ Atom)

| Class | Derives | PoC responsibility |
|---|---|---|
| `O3deRenderEngine` | `BaseRenderEngine` + `SingletonT` | `LoadImpl` boots the backend; `GraphicsAPI`→VULKAN; `Name`=="o3de" |
| `O3deRenderEnginePlugin` | `RenderEnginePlugin` | registration (`GZ_ADD_PLUGIN`) |
| `O3deScene` | `BaseScene` | create camera/visual/box/sphere/cylinder/material; tag geometry type |
| `O3deNode`/`O3deVisual` | `BaseNode`/`BaseVisual` | world transforms feeding the draw list |
| `O3deMaterial` | `BaseMaterial` | diffuse/ambient colour only |
| `O3deCamera` | `BaseCamera` | owns the render target; pose+projection per frame |
| `O3deRenderTarget` | `BaseRenderTarget` | **`Copy(Image&)`** → gather scene → `O3deBackend::RenderFrame` |
| `O3deGeometry` | `BaseGeometry` | records primitive type for AuxGeom |

**Primitives use AuxGeom, not cooked meshes:** `AuxGeomDraw` provides
`DrawAabb` (box), `DrawSphere`, `DrawCylinder`, `DrawCone` with built-in shaders,
avoiding the per-mesh/per-material asset-cooking pipeline. This is the single
biggest risk-reducer.

## Architecture: the Atom backend (`O3deBackend.cc`)

* All Atom/AzCore code is isolated in `O3deBackend.cc`, compiled with O3DE's
  compile model (clang, C++20, `-fno-exceptions`, O3DE defines) via per-source
  CMake properties. The rest of the component is ordinary gz-rendering C++ that
  talks to it through the plain-C++ `O3deBackend` pimpl interface.
* The backend hosts an `AzGameFramework::GameApplication` in-process, which
  dlopens the Atom gems and builds an offscreen `MainPipelineRenderToTexture`
  pipeline + perspective `View`. It is brought up once and **never torn down**
  (teardown of a live `RPISystem` crashes in Vulkan teardown). At process exit
  the render thread is joined (removing the only teardown race we own) and an
  `atexit` handler `std::quick_exit()`s to skip the upstream O3DE/NVIDIA
  GPU-teardown destructors, which crash whether the runtime is torn down or
  leaked. See `O3deBackend::Bootstrap()` for the full rationale.
* **All O3DE work — bootstrap and every render tick — runs on a single
  dedicated thread.** This is mandatory: Atom's `AssetManager` only self-pumps a
  synchronous (critical-shader) load when it runs on the thread that created the
  application. Driving ticks from gz-gui's render thread instead deadlocks on a
  blocking shader load during a pass-tree rebuild. `RenderFrame()` (called on
  gz-gui's thread) hands the camera + shapes across a mutex and waits for the
  latest readback.
* Per frame: point the `View` at the gz camera pose/projection, submit AuxGeom
  draws for the scene primitives, render, and read the pipeline's `Output`
  attachment back to CPU via `FrameCaptureRequestBus` → fill the gz `Image`.
* Anti-aliasing: the pipeline enables 4x MSAA (for future real meshes), but
  AuxGeom is drawn post-resolve at 1 sample, so the PoC primitives are smoothed
  by 2x **supersampling** instead — render at 2x the requested resolution and
  box-downsample on readback (see `kSsaaScale` in `O3deBackend.cc`).

### Coordinate mapping (verified)

gz cameras look down +X (REP-103: +X fwd, +Y left, +Z up); Atom views look down
+Y, +Z up; both right-handed Z-up. Object world poses pass through unchanged;
only the camera gets a local −90° Z yaw:
`o3de_cam_quat = GzQuat(camQuat) * Quaternion::CreateRotationZ(-HalfPi)`.
gz HFOV → vertical FOV via `vFov = 2*atan(tan(hFov/2)/aspect)`.

## CMake / build integration

* Top `CMakeLists.txt`: `option(BUILD_O3DE OFF)` — gated off, never affects a
  normal build. When on: verify `vendor/o3de`, set `GZ_RENDERING_HAVE_O3DE`,
  append `o3de` to the component list.
* `include/gz/rendering/config.hh.in`: `GZ_RENDERING_HAVE_O3DE`.
* `src/RenderEngineManager.cc`: `o3de` → `gz-rendering-o3de` default-engine entry.
* `o3de/src/CMakeLists.txt`: per-source compile model on `O3deBackend.cc`; link
  whole-archive `libAzGameFramework.a` + the Atom/AzCore/AzFramework libs.

## Milestones

| # | Milestone | Status |
|---|---|---|
| **M0** | gz plumbing with a stubbed engine (no O3DE); `Copy()` fills a gradient; verify load + gz-gui fallback end-to-end | **Done** |
| **M1** | O3DE offscreen render + readback **standalone** (no gz) — feasibility gate: build O3DE here, headless Vulkan offscreen, AuxGeom, readback→PNG | **Done** |
| **M2** | Fuse M0+M1 — real bootstrap/readback behind `O3deRenderTarget::Copy`; one hardcoded AuxGeom box in gz-gui via O3DE | **Done** |
| **M3** | Wire the scene graph — `Create{Box,Sphere,Cylinder}Impl` + transforms + material colour drive AuxGeom from real scene contents | **Done** |
| **M3.5** | Live gz-gui display — dedicated O3DE thread fixes the cross-thread asset-load deadlock; dynamic resize to the window's true aspect; ~60 fps continuous | **Done** |
| **M4** | (post-PoC) zero-copy interop for performance. Realized as **native Vulkan→Vulkan** (Qt on its Vulkan RHI imports Atom's image onto its `VkDevice`), not Vulkan↔GL. **Done & verified:** FD export (`vkGetMemoryFdKHR`), import onto another `VkDevice`, the static-probe display (#24), the render-finished timeline semaphore (#26), and the **live zero-copy display** (#25) — the scene renders into the shared image and Qt samples it zero-copy, verified ~2000 frames at ~60-100 fps across multiple window resizes with zero device loss. The earlier live device loss was a **consumer-side resize/re-import use-after-free** (freeing an import under Qt's in-flight frame), fixed by retiring old imports; the "compression handoff" and "missing semaphore" theories were both disproven en route (see findings doc). See [M4_INTEROP_DESIGN.md](M4_INTEROP_DESIGN.md), [docs/zero-copy-interop-findings.md](docs/zero-copy-interop-findings.md), [docs/task-tracker.md](docs/task-tracker.md) | **Done** |

## Risks & mitigations (outcomes)

1. **O3DE won't build in-tree** — *proven OK* in M1; kept `BUILD_O3DE` off by default.
2. **RPISystem needs cooked assets even for AuxGeom** — ship a cooked project
   (`GzAtomPoc`); reuse `MainPipelineRenderToTexture`.
3. **Headless Vulkan offscreen** — use the render-to-texture pipeline; *works*.
4. **Async readback vs synchronous `Copy()`** — block on the capture callback;
   gz-gui already warns this path is slow.
5. **AzCore lifetime vs dlopen'd plugin** — bootstrap once, never tear down.
6. **Cross-thread asset loads** — *the key Phase-1 bug*: run all Atom work on one
   dedicated thread so `AssetManager` self-pumps loads (see Architecture).

## Future work (beyond Phase 1)

* Remove debug logging in `O3deBackend.cc` / `O3deRenderTarget.cc`.
* Real meshes, textures, PBR materials, lights and shadows.
* Zero-copy interop, M4 — realized as **native Vulkan→Vulkan** and **working** (see
  [M4_INTEROP_DESIGN.md](M4_INTEROP_DESIGN.md) and
  [docs/zero-copy-interop-findings.md](docs/zero-copy-interop-findings.md)). FD export
  + import-onto-another-`VkDevice` + the static-probe display + the render-finished
  timeline semaphore (#26) + the **live zero-copy display** (#25) are done & verified
  behind `-DGZ_O3DE_INTEROP=ON` (needs the small gem patch in `patches/`), gated at
  runtime by `GZ_O3DE_INTEROP_LIVE` + `GZ_O3DE_INTEROP_SEM`. Possible future hardening:
  a consumer→producer "done sampling" edge / double-buffering (today a single shared
  image + producer host-sync + render-finished semaphore), and bounded reclaim of
  retired consumer imports.
* Replace hard-coded `vendor/o3de` + `~/o3de-packages` build paths with cache
  variables; ship a minimal vendored asset bundle instead of a full project.
