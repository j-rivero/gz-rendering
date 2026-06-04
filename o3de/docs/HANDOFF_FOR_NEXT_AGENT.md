# Handoff: o3de render-engine plugin for gz-rendering

Single-file primer so another Claude session (Opus 4.8 or later) can join
this proof-of-concept without re-reading every commit. Read this top to
bottom; everything else is reference linked from here.

Last updated: **2026-06-04** by Opus 4.8. Tag: **`o3de-poc-beta3`**
(real-asset PBR). **M13 real-asset PBR landed and user-verified live**: the
backend auto-applies the material a mesh FILE carries when no explicit gz
material is set — GLB/.dae files render fully textured (jetty Forklift:
orange body, steel-blue mast, rubber tires, all 4 maps from memory) vs
all-grey before. Two post-M13 hardening commits came out of live user
inspection: `61d3f2ab` adds the **`GZ_O3DE_DEMO_MESH_FILE` demo aid** (drop
any real asset into the live demo, floor-rested + slow-spinning;
`GZ_O3DE_DEMO_MESH_FILE_POS="x y"` moves it; pair with
`GZ_O3DE_DEMO_PBR_ONLY=1 GZ_O3DE_DEMO_NO_PBR=1
GZ_O3DE_DEMO_NO_SHADOWCASTER=1` for clean inspection) and `fea397ae` fixes
the **doubleSided gotcha**: glTF assets author doubleSided=true thin shells
(Forklift wheel faces/chassis panels vanished under back-face culling), and
gz-common drops the glTF flag for OPAQUE materials (AssimpLoader reads
AI_MATKEY_TWOSIDED only in the alphaMode=MASK branch), so auto-applied file
materials now set StandardPBR `general.doubleSided=true` unconditionally.
M12 real gz materials (public API chain) and M11 PBR factors/textures/IBL
were already complete. The long-standing **runtime-mesh-unlit bug is fully
root-caused & fixed** (inverted shading normals vs reversed winding — commit
`075caed7`). All shadow work (spot + directional cascade) was already complete.
Since the 2026-06-01 handoff: M7 spot light direction fixed; M8 cascade
shadows; single-light-greys disproven; M11 PBR; runtime-mesh-unlit RESOLVED;
M12 gz-API material chain; **M13 mesh-file material auto-apply +
double-sided fix + live-demo mesh-file aid**.

---

## TL;DR

We are building a **third render-engine plugin for `gz-rendering`** (the
existing two are OGRE and OGRE2) that wraps **O3DE / Atom** — the
renderer from Open 3D Engine — and integrates with gz-gui's
`MinimalScene` widget over a **native Vulkan-to-Vulkan zero-copy** path
(Atom renders on its own `VkDevice`, Qt imports the colour image onto
the Qt RHI device and samples it with `QSGSimpleTextureNode`).

The work is on branch `o3de_render_engine_poc` of
`/home/jrivero/code/gz/gz-rendering` (PR base: `gz-rendering9`).

Status today: **milestones M0–M7** are landed. The demo at
`o3de/examples/native_vulkan_live/live_demo.sh` boots Atom in
`--console-mode`, builds a scene with AuxGeom primitives (M0–M3) +
M5 visuals (capsule, arrow, axis, frustum) + M6 dynamic lights (point +
spot) + M7 cooked Mesh assets (sphere caster + flat-sphere ground
receiver) + a `ProjectedShadow` projector and per-spot shadow
configuration. **Camera orbit/pan/zoom is interactive** via the
`InteractiveViewControl` gz-gui plugin.

Update (2026-06-03, Opus 4.8): the two formerly-open bugs are resolved.
(1) "single-light greys the render" was disproven — it is the known
intermittent QSG/present-race grey, not light count. (2) "spot tint +
shadow not visible" is fixed: the spot was emitting ZERO light because
it was aimed along local -Z while Atom's SimpleSpotLight emits along +Z
(`transform.GetBasisZ()`); fixed in commit `8e717897`. The spot's cast
SHADOW is the one remaining smaller follow-up. Also landed this session:
the runtime-mesh-unlit bug is RESOLVED (was light-facing, not a renderer
bug) and the hero-box relight is promoted to the demo default. See "Open
investigations" below and the memory entries for full detail.

## Where things live

```
/home/jrivero/code/gz/gz-rendering/        # the repo, branch o3de_render_engine_poc
├── o3de/                                  # OUR PLUGIN COMPONENT
│   ├── include/gz/rendering/o3de/         # public headers (24 .hh files)
│   ├── src/
│   │   ├── O3deBackend.cc                 # ~2700 lines: Atom bootstrap,
│   │   │                                   #  SetupScene (2 paths: interop +
│   │   │                                   #  CPU readback), MaybeInjectDemo*,
│   │   │                                   #  SubmitLights, AcquireDemoMeshes
│   │   ├── O3deRenderTarget.cc            # GatherFrame: gz scene graph
│   │   │                                   #   -> O3deShapeData[] +
│   │   │                                   #      O3deLightData[]
│   │   ├── O3deScene.cc                   # Create*Impl factories
│   │   ├── O3deCamera.cc                  # WorldPose -> O3deCameraData
│   │   └── ...
│   ├── examples/native_vulkan_live/
│   │   ├── live_demo.sh                   # canonical launcher
│   │   └── live_demo.config               # gz-gui plugin list
│   ├── examples/pbr_materials/            # M12: headless API-sourced roughness sweep
│   ├── examples/mesh_pbr_viewer/          # M13: load any mesh file + auto-frame + PNG
│   └── docs/
│       ├── HANDOFF_FOR_NEXT_AGENT.md      # THIS FILE
│       ├── MANUAL_VERIFY_M5_M7.md         # checklist for visual demo verification
│       ├── architecture.html              # "how a pixel reaches your screen"
│       ├── diagnostic-tools.md            # WSI primer + swapchain dump usage
│       ├── zero-copy-interop-findings.md  # M4 zero-copy bug-hunt journal
│       └── screenshots/                   # reference captures (m7-*.png etc.)
└── vendor/o3de/                           # 5.1 GB vendored O3DE clone
                                            # (Atom Gems, AzCore, etc.)
```

Out-of-tree but constantly used:

| Path | What |
|---|---|
| `/home/jrivero/code/gz/ws_o3de_rendering` | colcon workspace; `install/` is what the demo loads. |
| `/home/jrivero/code/gz/gz-rendering/build_o3de` | direct CMake build dir we iterate in (faster than colcon). After each rebuild, `cp build_o3de/lib/libgz-rendering-o3de.so.11.0.0~pre1` over BOTH copies in `ws_o3de_rendering/install/lib/` (`lib/` and `lib/gz-rendering/engine-plugins/`). |
| `/home/jrivero/code/gz/gz-gui` | gz-gui (Vulkan-RHI patched build is in `ws_o3de_rendering/install`). |
| `/home/jrivero/o3de-gzpoc/Cache/linux` | the cooked-asset cache the engine loads on startup. Has `sphere.fbx.azmodel`, `occlusioncullingplane.fbx.azmodel`, `basic_grey.azmaterial`, all the shaders, and the pass templates. NO new assets have been cooked since M0. |
| `/tmp/o3de_debug/` | session-scratch: `m7_visual.log`, screenshots, verifier scripts |
| `/tmp/gz_swapchain_prime.frame_*.ppm` | post-QSG pre-`vkQueuePresentKHR` dumps from the WSI primer layer — use these to distinguish "Atom rendered wrong" from "Qt sampled wrong" when the viewport goes grey |
| `~/.claude/memory/project_o3de_*.md` | persistent project memory (6 entries; see "Memory" below) |

## How to build and run

The base assumption is that the colcon workspace is already built. You
only need to recompile the `gz-rendering-o3de` target when you change
`o3de/src/*.cc` or `o3de/include/...`.

```bash
# 1. Build the o3de backend incrementally (~15 s clean, ~5 s touch-up)
cd /home/jrivero/code/gz/gz-rendering/build_o3de
cmake --build . --target gz-rendering-o3de -j5

# 2. Install over the workspace (BOTH paths or the demo gets a stale .so)
SRC=/home/jrivero/code/gz/gz-rendering/build_o3de/lib/libgz-rendering-o3de.so.11.0.0~pre1
WS=/home/jrivero/code/gz/ws_o3de_rendering/install/lib
cp -f "$SRC" "$WS/libgz-rendering-o3de.so.11.0.0~pre1"
cp -f "$SRC" "$WS/gz-rendering/engine-plugins/libgz-rendering-o3de.so.11.0.0~pre1"

# 3. Run the demo (will window onto :1)
/home/jrivero/code/gz/gz-rendering/o3de/examples/native_vulkan_live/live_demo.sh
```

The demo sets `GZ_O3DE_INTEROP=1`, `GZ_O3DE_INTEROP_LIVE=1`,
`GZ_O3DE_INTEROP_SEM=1`, `GZ_O3DE_DEMO_SHAPES=1` and points
`GZ_RENDERING_PLUGIN_PATH` + `GZ_CONFIG_PATH` at the colcon install.
It also enables the WSI swapchain dump layer if found.

## Milestones

The PoC roadmap from `~/.claude/plans/compiled-napping-quail.md`
(M0–M3 were laid out there) plus the post-beta1 milestones M4–M7
that came after. Status as of 2026-06-01:

| ID | What | Status |
|----|------|--------|
| M0 | gz plumbing with a STUB engine (filled gradient via `Copy()`); proves load + scene + camera + gz-gui fallback end-to-end | ✓ landed |
| M1 | Standalone Atom offscreen + readback as a feasibility gate | ✓ subsumed by M2 (didn't need a separate scratch exe) |
| M2 | Fuse M0+M1: real Atom render writing into the readback Image | ✓ landed |
| M3 | Scene graph: `Create{Box,Sphere,Cylinder,Grid,Plane,WireBox}Impl` + `O3deVisual/Node` transforms + `O3deMaterial` color drive AuxGeom from actual scene contents | ✓ landed |
| M4 | Native Vulkan→Vulkan zero-copy via `VkImportMemoryFdInfoKHR`; consumer (gz-gui) imports the Atom image onto its `VkDevice` and samples directly. Working in `GZ_O3DE_INTEROP_LIVE=1`. | ✓ landed; had a long grey-display bug-hunt (QSG sampling race); see `zero-copy-interop-findings.md` |
| M5 | Capsule + Mesh-no-op + ArrowVisual + AxisVisual + FrustumVisual primitives | ✓ A/B/C/D all landed |
| M6 | Atom `LightFeatureProcessor` integration (point + spot light handles, per-frame `Set*` sync) | ✓ A + C landed; B was folded into A |
| M7 | Shadows: cache `ProjectedShadowFP`, paired projected-shadow handle per spot, cooked Mesh caster + receiver, `SetShadowsEnabled` on `SimpleSpotLight` | ✓ A1, A2, B, C landed. Spot **cone tint visible** (direction bug fixed `8e717897`). Cast **shadow RESOLVED** (`d4e468eb`): shadows DO render — a cooked sphere AND the runtime hero box both cast clear shadows when floated over open floor (`screenshots/m7-shadow-ab-both-cast.png`). The "not visible" was scene placement/occlusion, not a render bug; material-variant hypothesis refuted. Cleanup TODO: the manual A2 ProjectedShadow is redundant (SimpleSpotLight owns its own). |
| M8 | Directional "sun" light (`DirectionalLightFeatureProcessor`) + **cascade shadows** | ✓ light landed `540471f1`; **cascade shadows landed** (`o3de: M8 — directional sun CASCADE shadows`). Per-frame `SetCameraConfiguration`+`SetCameraTransform` (cascades fit the live camera), one-time `SetShadowEnabled`/`SetShadowmapSize(1024)`/`SetCascadeCount(2)`/`SetShadowFarClipDistance(30 m)`/PCF. Verified in isolation (`screenshots/m8-directional-cascade-shadow.png`) and in the default demo alongside the spot shadow (`screenshots/demo-both-shadows-spot-and-sun.png`). Sun 14 lux, dir `(-0.35,0.15,-0.925)`. Perf: shadow pass scales with render res (~33 ms default window, ~85 ms maximized); `GZ_O3DE_DEMO_NO_SUN_SHADOW=1` drops just the sun shadow. **All shadow work is now complete.** |
| M11 | **Real PBR materials & textures** on runtime meshes | ✓ **Phase A** (`075caed7`): `O3deMeshData` gains `metallic`/`roughness`; `SubmitMeshes` sets `metallic.factor`/`roughness.factor` per-mesh. Demo roughness sweep (5 spheres .05→.95) + gold/steel pair. **Also root-caused & FIXED the long-standing runtime-mesh-unlit bug** (see below). ✓ **Phase B** (`1060d5b1`): base-color texture — `DemoBaseColorImage()` builds a checkerboard `StreamingImage` via `CreateFromCpuData`, bound to `baseColor.textureMap`+`useTexture` on `textured=true` meshes; textured sphere UV-maps + shades correctly. Isolate with `GZ_O3DE_DEMO_PBR_ONLY=1`. Screenshots `m11-pbr-*.png`. ✓ **Phase C** (`ab66c94f`): **IBL cubemap** — `SetupIbl()` feeds the registered `ImageBasedLightFeatureProcessor` the project's cooked default IBL cubemaps (`lightingpresets/default_iblskyboxcm_{iblspecular,ibldiffuse}.exr.streamingimage`); metals go from near-black to convincing reflective metal (gold warm, steel = chrome mirror — `m11-pbr-ibl-ab.png`). IBL washes the tuned shadow contrast, so it's **default-on only in PBR_ONLY**; the shadow demo keeps it off unless `GZ_O3DE_DEMO_IBL=1` (`GZ_O3DE_DEMO_NO_IBL=1` force-off, `GZ_O3DE_IBL_EXPOSURE` tunes). ✓ **Phase D** (`d8a5c0ce`): **real albedo texture FILES** — `O3deMeshData.texturePath` decoded at runtime by `FileBaseColorImage()` via `gz::common::Image` (already linked) → `CreateFromCpuData`, cached per path; takes precedence over the checker. The demo ships `assets/gz_albedo_demo.png` and points `GZ_O3DE_DEMO_TEXTURE` at it; the decoded image UV-wraps the sphere (text visibly wraps) and coexists with cast shadows (`m11-pbr-texture-file*.png`, `m11-pbr-full-demo-textured.png`). This is the exact path a real gz material's base-color map takes. |
| M12 | **Real gz materials on runtime meshes** — the public-API chain | ✓ landed (`58da3c50`..`b9de0cba`). `GatherFrame`'s mesh branch copies `Metalness()`/`Roughness()`/`Texture()` from the attached gz material into `O3deMeshData` (backend consumes them unchanged since M11). TWO latent base-class traps fixed along the way: (1) `BaseMaterial`'s PBR setters are no-ops — `O3deMaterial` now stores texture/roughness/metalness itself (`403a786b`; defaults metalness 0 + roughness 1 so color-only materials read plain diffuse); (2) `BaseMesh::Material()` reads submesh 0 and returns null on an empty store — `O3deMesh::Material()` overrides to return the mesh-level material (`41eadce4`). Proven by `o3de/examples/pbr_materials/` — a headless example building the scene 100% via the public gz API (CreateMesh + CreateMaterial + CreatePointLight, no demo injection, no Qt) and capturing through the CPU-readback path; fails loudly (exit 1) if the backend never renders. Verified capture: `m12-pbr-materials-gz-api.png` (roughness sweep, gold, file-textured sphere). Scope: mesh geometries only (primitives stay AuxGeom); proven property set only. Per-submesh materials still no-op. Spec: `o3de/docs/specs/2026-06-04-m12-real-gz-materials-design.md`. |
| M13 | **Real-asset PBR** — mesh-file materials auto-apply when no explicit gz material is set | ✓ landed (`b9e3b651`..`0107ce99`). `RegisterMesh` extracts `MeshFileMaterialCpu` on the gz thread (`material[0]`; multi-material one-shot warning; TANGENT-only normal maps). `StreamingImageFromCommonImage` helper + sRGB-keyed caches (FileTexture path+srgb / MemTexture image-address+srgb) wire albedo as sRGB and normal/metal/rough as UNORM-linear (avoiding the washed-out-normals trap). `SubmitMeshes` applies via an M12-sentinel gate: `metallic>=0 || roughness>=0 || texturePath nonempty || textured` = explicit gz material wins entirely; otherwise the file-extracted maps are bound. One-shot telemetry: `[gz-o3de] M13 file-material id=... albedo=mem|path|none|fail ...`. Verified: jetty Forklift `base_visual.glb` renders fully textured (orange body, steel-blue mast, rubber tires; albedo=mem normal=mem metal=mem rough=mem) vs all-grey before (`screenshots/m13-forklift-textured.png`, `m13-forklift-before-grey.png`); `cordless_drill.dae` proves path-based sidecar form (albedo=path); `pbr_materials` regression unchanged (explicit materials still win). New example: `o3de/examples/mesh_pbr_viewer/` — `mesh_pbr_viewer.sh <mesh-file> [out.png]`, headless CPU-readback PNG, no SetMaterial needed. Known limitations: material[0] only (per-submesh slots future); no emissive/occlusion maps; `O3deMesh::Material()` does not reflect the auto-applied file material; gz-API in-memory `SetTexture(_, img)` still ignores the image arg. Spec: `o3de/docs/specs/2026-06-04-m13-real-asset-pbr-design.md`; plan: `o3de/docs/plans/2026-06-04-m13-real-asset-pbr.md`. **Post-landing hardening** (user live inspection): `61d3f2ab` `GZ_O3DE_DEMO_MESH_FILE` live-demo aid; `fea397ae` auto-applied file materials render double-sided (`general.doubleSided=true`) — glTF thin-shell assets (Forklift wheels/chassis) vanish under back-face culling and gz-common drops the glTF doubleSided flag for OPAQUE materials (AssimpLoader reads AI_MATKEY_TWOSIDED only in the alphaMode=MASK branch); `4c0990bd` refreshed forklift screenshot. |

### Commit stack (most recent on top)

```
ac095ad6  o3de: M7 Phase C — visual tuning + flat receiver + grey material
6b159c83  o3de: M7 Phase B — demo meshes (caster + receiver) for shadow pass
8163fe8d  o3de: M7 Phase A2 — paired ProjectedShadow per spot light
0db635e4  o3de: M7 Phase A1 — register + cache ProjectedShadowFP
d13eaedd  o3de: M6 Phase C — demo lights + SubmitLights() telemetry
dbc2d41f  o3de: M6 Phase A -- Atom LightFeatureProcessor integration
b3a3cd59  o3de: M5 Phase C+D -- ArrowVisual + AxisVisual + FrustumVisual
03e30fc0  o3de: M5 Phase B -- no-op Mesh fallback so ArrowVisual::Init succeeds
6bcc0262  o3de: M5 Phase A -- Capsule primitive (cylinder body + sphere caps)
[earlier: M0–M4 work + extensive grey-display bug-hunt commits]
```

`git log --oneline o3de_render_engine_poc` shows the full ~50-commit history.

## Open investigations (carry forward)

Two memory entries describe these in full reproduction detail. Read
them before re-attempting either; both have hypothesis lists for the
"one variable at a time" tests that come next.

| Memory entry | Status |
|---|---|
| [`o3de-m7-spot-shadow-not-visible`](file:///home/jrivero/.claude/memory/project_o3de_m7_spot_shadow_not_visible.md) | **Light: RESOLVED** (commit `8e717897`). The spot emitted zero light — aimed local -Z but Atom's SimpleSpotLight emits along +Z (`transform.GetBasisZ()`); the cone pointed 180° away. Fixed with a code-computed look-at `q:(0,0,+1)->dir`; spot now lights a clear blue cone (`srgb(0,0,0)`->`srgb(195,212,233)`). The 6 old hypotheses were all wrong. **Cast SHADOW also RESOLVED** (commit `d4e468eb`): shadows DO render — a cooked sphere AND the runtime hero box both throw clear black cast shadows when floated over open, spot-lit floor (`GZ_O3DE_DEMO_ONLY_LIGHT=spot` + `GZ_O3DE_COOKED_CASTER`/`GZ_O3DE_CASTER_POS`/`GZ_O3DE_HERO_POS`; `screenshots/m7-shadow-ab-both-cast.png`). The "not visible" was scene placement/occlusion; the material-variant hypothesis is refuted (runtime mesh casts fine). Cleanup TODO: the backend's manual A2 ProjectedShadow is redundant (SimpleSpotLight owns its own shadow). |
| [`o3de-single-light-greys-render`](file:///home/jrivero/.claude/memory/project_o3de_single_light_greys_render.md) | **DISPROVEN** (2026-06-02). Not a light-count bug — it's the intermittent QSG/present-race grey that strikes regardless of 1 vs 2 lights or shadow on/off. The capture harness now grey-detects (centre-crop colors==1) and retries. |
| [`o3de-runtime-mesh-unlit`](file:///home/jrivero/.claude/memory/project_o3de_runtime_mesh_unlit.md) | **RESOLVED — REAL ROOT CAUSE found** (commit `075caed7`). Runtime meshes shaded black because their **shading normals were INVERTED** relative to the reversed triangle winding: StandardPBR's pixel-stage front-face flip negated the (correct, outward) normal → `NdotL<0` → black + Fresnel rim. Fix = negate the vertex normal in `ExtractMeshGeometry` (opt out `GZ_O3DE_MESH_NO_FLIP_NORMALS`). Proven by isolated sphere A/B (`m11-runtime-mesh-unlit-fix-ab.png`). The earlier "light-facing / BOXFILL relight" verdict (`0aa2ef9e`, `f4b9d7a8`) was a workaround that masked this — a box has too few orientations to expose it; a sphere did. BOXFILL is now just cosmetic fill (cleanup TODO to drop it). The prior RenderDoc "normal is correct" read was the *post-VS* normal (pre-flip). |

Everything in this table is closed. Remaining cleanup TODOs: (1) the manual A2 ProjectedShadow (redundant); (2) the now-cosmetic BOXFILL relight; (3) the inert emissive crutch.

## How the demo scene maps to milestones (visual verify guide)

When the demo is running with the default camera pose, the camera looks
at the world origin from `(-4, 0, 1.2)` pitched down `0.12 rad`. After
the scene reorg + the M11 showcase-shelf polish (`a600e522`, see
`screenshots/demo-m11-showcase-shelf.png` for the exact expected frame)
you should see:

* **M11 showcase shelf** floating high across the frame (x=0.9, z=2.6):
  [gold | roughness sweep .05→.95 | steel] — 7 runtime spheres, metals
  bookending the sweep (metals read dark without IBL; set
  `GZ_O3DE_DEMO_IBL=1` to make them reflect)
* M0/M3 front row on the floor, left→right: red box (spinning in
  place), green sphere (bobbing) with its yellow wirebox cage, enlarged
  RGB axis gnomon at the clear origin, blue cylinder (spinning in
  place), magenta capsule (tilting)
* **M9/M10 cyan hero box** (runtime gz-common mesh) spinning behind the
  origin, with the small white satellite orbiting above it at z=1.55
* **M7/M11-D textured sphere** front-right at (-1.0,-1.6) — the real
  albedo PNG UV-wrapped (gz "swirl" image)
* **M7 white cooked caster sphere** floating left-of-centre in the spot
  beam (its blue beam marker line crosses the frame) with its **cast
  shadow** on the open floor below-right of it
* M5-D cyan frustum wireframe (top-right), M0/M3 orange backdrop wall,
  grey grid floor, M7-B flat-sphere ground disc under everything
* (Subtle) M6-C warm point-light tint on upward-facing surfaces

The full checklist is `o3de/docs/MANUAL_VERIFY_M5_M7.md` (12 items,
camera + telemetry).

Reference captures:

* `o3de/docs/screenshots/demo-m11-showcase-shelf.png` — current default
  frame after the M11 shelf polish (the layout described above)
* `o3de/docs/screenshots/m7-phase-b-meshes.png` — pre-M7-C, white sphere
  visible on top of green (historical layout)
* `o3de/docs/screenshots/m7-phase-c-cranked-lights.png` — M7-C, brighter
  lighting, no ground disc yet (historical layout)
* `o3de/docs/screenshots/m7-flat-receiver-disc.png` — final M7-C with
  flat receiver disc visible (historical layout)

## Architecture quick-reference

For "how does a pixel actually get to the screen" see
`o3de/docs/architecture.html` (a long-form explainer written during the
M4 zero-copy bug-hunt). One-paragraph version:

> gz-gui's `MinimalScene` plugin owns the camera and the Qt window.
> Each frame, our `O3deCamera::Render()` (or the readback path) drives
> `O3deBackend::RenderFrame` / `RenderFrameForInterop`, which posts a
> `pendingCamera` + `pendingShapes` + `pendingLights` snapshot to a
> render thread. The render thread runs Atom's `RPISystem::SimulationTick`
> + `RenderTick` on a `MainPipelineRenderToTexture` pipeline that writes
> into an **exportable Vulkan image** (created with
> `VkExternalMemoryImageCreateInfoKHR`+`OPAQUE_FD`). The image FD is
> passed to the gz-gui side, which `vkAllocateMemory` + `VkImportMemoryFd`s
> it onto Qt's `VkDevice`, then a `QSGSimpleTextureNode` samples it into
> the Qt scene graph. **Atom and Qt never share a `VkDevice`** — only
> the image memory and a timeline semaphore.

## Key code paths

When you next dig in, these are the lines you most likely want:

| Want to change | Look at |
|---|---|
| Add a primitive | `O3deBackend::Impl::SubmitPrimitives` (the big switch over `O3deShapeData::Type`), then `O3deRenderTarget::GatherFrame`'s `ToBackendType`. Mirror an existing case (CAPSULE is recent and clean). |
| Add a light type | `O3deLightData::Type` enum in `O3deBackend.hh` + `O3deBackend::Impl::SubmitLights` switch + gather loop in `O3deRenderTarget::GatherFrame`. |
| Add a feature processor | Append to the `featureProcessors` vector at `O3deBackend.cc:518` (both `SetupScene` paths). Cache the pointer right after `scene->Activate()`. |
| Add a cooked Mesh | `O3deBackend::Impl::AcquireDemoMeshes` for demo-time; for runtime meshes, route through `O3deScene::CreateMeshImpl` (which today returns a no-op stub). |
| Wire shadow on a light | `SubmitLights`'s first-sighting block; for spots use `spotLightFp->SetShadowsEnabled(handle, true)` — Atom's `SimpleSpotLight` has its own self-contained shadow path, **not** `ProjectedShadowFP`. The `ProjectedShadowFP` we wire in M7-A2 is a parallel projector system for decals. |
| Change demo content | `MaybeInjectDemoShapes` (~2000) and `MaybeInjectDemoLights` (~2400) in `O3deBackend.cc`; both no-op unless `GZ_O3DE_DEMO_SHAPES=1`. |
| Add gz-gui plugin to the demo | edit `o3de/examples/native_vulkan_live/live_demo.config`. The repo file is what runs; no install step needed. |

## Verification harness

The reusable bits live in `/tmp/o3de_debug/` (recreated each session).
The pattern that works:

1. Launch demo backgrounded with stderr to a log file.
2. Sleep 14–22 s for warmup (engine boot ~3 s, plus QSG first-resize regen).
3. Find the right Qt window via `wmctrl -l` + `xprop _NET_WM_PID` and walk the parent chain to the demo PID (multiple `Gazebo GUI` windows may exist).
4. Mouse-jiggle to keep the Qt scheduler ticking, then `import -window $WID` to capture.
5. Grep the log for the telemetry lines listed in the M7 commits (`M6 light FPs cached`, `M7 demo mesh acquired`, `M7 spot shadow acquired`, etc.).
6. `compare -metric AE` between two captures 3 s apart proves animation.

DO NOT capture with `-window root` on `:1` — that captures the host
desktop, not the demo. Always window-scoped via the discovered `$WID`.

DO NOT use Xvfb for this work — Vulkan there falls back to `lavapipe`
which is an entirely different code path (the project has a memory
entry [`screen-capture-rules`](file:///home/jrivero/.claude/memory/feedback_screen_capture_rules.md)
explaining the trade-off).

## Memory entries to read

```bash
ls ~/.claude/memory/project_o3de_*.md
# project_o3de_grey_when_no_monitor.md       # x-display dependency of QSG
# project_o3de_live_demo_grey_display.md     # the older intermittent QSG bug
# project_o3de_m7_spot_shadow_not_visible.md # open investigation #1
# project_o3de_render_plugin.md              # original PoC kickoff context
# project_o3de_rendering_roadmap.md          # high-level milestone status
# project_o3de_single_light_greys_render.md  # open investigation #2
```

Plus the index at `~/.claude/memory/MEMORY.md`.

## Constraints / conventions to honour

These are baked into the global CLAUDE.md and are project-level rules
that have already saved time during this work:

* **Always use `git ci` for commits** (alias for `commit --signoff`),
  with author/committer email `jrivero@honurobotics.com`. End commit
  messages with `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`
  or the equivalent for whichever model is committing.
* **Never `rm -rf` to clean up** — move to `/tmp/` instead.
* **Always confirm before `git push`**, even in YOLO sessions; local
  commits are fine. Today's stack is local-only by design.
* **Build parallelism**: this project tolerates `-j5` well on this host
  (NVIDIA RTX, 6144x3456 framebuffer). The Atom subtree compiles
  many huge templates — don't push to `-j$(nproc)`.
* **Screen capture is allowed only window-scoped on `:1`**, never
  `-window root`. Memory entry [`screen-capture-rules`] enforces this.
* **Don't introduce premature abstractions**; the PoC is "make it
  work" territory. Bug fix ≠ tour-of-cleanups. The CLAUDE.md "do not
  half-finish, do not over-design" principles apply.

## Suggested next moves for the joining agent

In rough order of "most useful next":

1. **Reproduce the manual verify checklist** (`MANUAL_VERIFY_M5_M7.md`)
   on the running demo to confirm everything currently works. The
   camera now responds to mouse — orbit/pan/zoom should both verify
   M5/M6/M7 elements visually and confirm the InteractiveViewControl
   change works.
2. **Cleanup TODOs**: (a) the redundant manual A2 `ProjectedShadow`
   (SimpleSpotLight owns its own shadow path — the A2 projector is dead
   weight); (b) the now-cosmetic BOXFILL relight (was a masking workaround
   for the inverted-normals bug, no longer needed); (c) the inert emissive
   crutch. None of these is a blocker, but they add noise to future diffs.
3. **Per-submesh material slots (M13 follow-up).** Currently `material[0]`
   applies to the whole flattened mesh. Real GLB files carry per-submesh
   material assignments; routing them through `SubmitMeshes` (one Atom
   material handle per submesh draw call) would give full fidelity on
   multi-material assets like the jetty Forklift.
4. **Jetty-demo full-scene showcase.** With M13 landed, many jetty models
   (Forklift, crane, containers) can be loaded in a single scene via
   `mesh_pbr_viewer` or a new `jetty_demo` example, proving multi-asset
   PBR at scene scale with no gz-material setup.
5. **Emissive maps.** The StandardPBR shader supports `emissive.textureMap`
   + `emissive.factor`; the extraction and bind path mirrors the albedo
   path but needs the map pulled from `MeshFileMaterial::emissiveTextureImage`.
6. **Consume a real gz-sim scene end-to-end.** The gz-material chain is
   proven by M12 and the mesh-file material chain by M13. The next step
   is wiring a gz-sim world's actual scene/visual stream through
   `O3deScene::CreateVisualImpl` so a stock `shapes.sdf` world renders
   with real PBR materials. Also route primitives through the mesh path
   (currently they go via AuxGeom) so the `shapes.sdf` spheres/boxes get
   the same PBR treatment as mesh visuals.
7. **Qt 6.8 upgrade** to drop the DPR=1 + WSI-prime grey workarounds. See
   `live_demo.sh` comments (`QT_SCALE_FACTOR=1`, `QT_AUTO_SCREEN_SCALE_FACTOR=0`,
   the `VK_LAYER_GZ_swapchain_dump` MAX_FRAMES=4 fence fence-wait); commit
   `a15c3519` in the Qt tree covers the relevant QQuickRt change.
8. **Push to `j-rivero/gz-rendering`**: needs explicit user confirmation
   before pushing per the memory rule. No PR has been opened yet; all
   commits are local only.

## Hand-off contract

If you make changes, please:

* Add or update the relevant memory entry rather than letting it rot.
* Update this `HANDOFF_FOR_NEXT_AGENT.md` whenever a milestone lands.
* Keep the commit-message format consistent with what's on the branch
  (multi-paragraph body explaining *why*, code-only verification quote,
  Co-Authored-By trailer).
* When a new commit changes the open-questions list, update the "Open
  investigations" table here, NOT just the memory entry.

Good luck.
