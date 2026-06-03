# Handoff: o3de render-engine plugin for gz-rendering

Single-file primer so another Claude session (Opus 4.8 or later) can join
this proof-of-concept without re-reading every commit. Read this top to
bottom; everything else is reference linked from here.

Last updated: **2026-06-03** by Opus 4.8. Last commit on the branch:
`d4e468eb` (M7 cast shadow RESOLVED — placement artifact). Since the 2026-06-01 handoff:
runtime-mesh-unlit RESOLVED + relight promoted to default; M7 spot light
direction fixed (spot was emitting no light); single-light-greys disproven.

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
| M8 | Directional "sun" light (`DirectionalLightFeatureProcessor`) | ✓ landed `540471f1`. Enabled by default (was deferred at M6-A for a grey-screen that proved to be the FP-independent present race). 8 lux warm sun; `GZ_O3DE_DEMO_NO_SUN=1` / `GZ_O3DE_SUN_INTENSITY=<lux>`. Directional **cascade shadows** not yet wired (SetCascadeCount/SetShadowmapSize/SetCameraConfiguration) — now the **only** remaining shadow work, since the spot cast shadow is resolved (`d4e468eb`). |

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
| [`o3de-runtime-mesh-unlit`](file:///home/jrivero/.claude/memory/project_o3de_runtime_mesh_unlit.md) | **RESOLVED** (commits `0aa2ef9e`, `f4b9d7a8`). The runtime hero box rendering black under light was light-facing, not a renderer bug (proven via RenderDoc). Hero-box relight (2 opposed fills) promoted to demo default; emissive crutch dropped. |

Remaining: the spot's cast shadow (see above). Everything else here is closed.

## How the demo scene maps to milestones (visual verify guide)

When the demo is running with the default camera pose, the camera looks
at the world origin from `(-4, 0, 1.2)` pitched down `0.12 rad`. From
there you should see (left→right):

* M5-D cyan frustum wireframe (top-left)
* M0/M3 red box (spinning), green sphere (bobbing), blue cylinder
  (orbiting), yellow wirebox cage tracking the green sphere
* M5-C tiny RGB axis arrows at origin
* M0/M3 orange wall plane (back-centre, lit dim brown without a
  directional light)
* M5-A magenta capsule (right, slowly tilting)
* M0/M3 grey grid floor
* **M7-B white shaded PBR sphere** stacked above the green sphere — this
  is the cooked-mesh caster
* **M7-B solid lit ground disc** under all of it — the flat-sphere
  receiver
* (Subtle) M6-C warm yellow tint on the M7 caster's upward-facing cap —
  the point light contributing on the PBR surface

The full checklist is `o3de/docs/MANUAL_VERIFY_M5_M7.md` (12 items,
camera + telemetry).

Reference captures:

* `o3de/docs/screenshots/m7-phase-b-meshes.png` — pre-M7-C, white sphere
  visible on top of green
* `o3de/docs/screenshots/m7-phase-c-cranked-lights.png` — M7-C, brighter
  lighting, no ground disc yet
* `o3de/docs/screenshots/m7-flat-receiver-disc.png` — final M7-C with
  flat receiver disc visible

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
2. **Tackle [`o3de-single-light-greys-render`]** as a single-variable
   test: keep both lights but remove the `SetShadowsEnabled` call in
   `SubmitLights`. If the scene still renders, then shadow setup +
   1-light combination is the issue. If it greys, then it's purely a
   light-count thing and the spot's shadow isn't the cause.
3. After (2), revisit [`o3de-m7-spot-shadow-not-visible`] — if the
   1-light failure traces back to shadow plumbing, that may already
   explain why the receiver doesn't show the spot's contribution.
4. **Shadows (the remaining lighting work)**: neither the spot's cast
   shadow nor directional cascade shadows are visually confirmed. The
   spot/directional LIGHTS both work now; the shadow *passes* are the
   open piece. Start with a clean caster+receiver (a single PBR caster
   between the light and an otherwise-dim floor patch) and a RenderDoc
   capture to confirm the caster draws into the shadowmap; check whether
   runtime-built meshes register as shadow casters at all (cooked vs
   runtime, echoing the runtime-mesh saga). M8 directional sun itself is
   DONE (landed `540471f1`, on by default).
5. **Eventually push to `j-rivero/gz-rendering`**: needs explicit
   confirmation before pushing per the memory rule. No PR has been
   opened yet; commits are local only.

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
