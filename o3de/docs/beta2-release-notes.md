# O3DE/Atom gz-rendering PoC — beta2

Tag: `o3de-poc-beta2` (branch `o3de_render_engine_poc`, PR base `gz-rendering9`).
Supersedes `o3de-poc-beta1` (M4 zero-copy). 35 commits since beta1.

beta1 proved the native Vulkan→Vulkan zero-copy display path (Atom renders
offscreen on its own `VkDevice`; Qt imports the colour image and samples it —
no CPU readback, 60 FPS). beta2 builds the **scene + lighting** layer on top.

## What's new since beta1

### Scene primitives & meshes (M5, M9, M10)
- M5: capsule, arrow, axis, and frustum visuals.
- M9/M10: runtime `gz::common::Mesh` → Atom `ModelAsset` converter
  (`BuildModelAssetFromGeometry`), wired through `O3deScene::CreateMeshImpl`,
  with per-mesh material colour from the gz Visual.

### Lighting (M6, M7, M8)
- M6: Atom `LightFeatureProcessor` integration — point + spot light handles,
  per-frame transform/intensity sync.
- M7: spot light **cone tint now works** (see fix below); shadow scaffolding
  (`ProjectedShadowFP` + `SetShadowsEnabled`) in place.
- M8: directional "sun" light (`DirectionalLightFeatureProcessor`), on by
  default (8 lux warm fill; `GZ_O3DE_DEMO_NO_SUN=1` to omit,
  `GZ_O3DE_SUN_INTENSITY=<lux>` to tune).

### Bugs root-caused & fixed this pass (2026-06-03)
- **Runtime mesh rendered black under lighting — RESOLVED.** Proven via a
  RenderDoc in-app capture path (the only way to capture this two-VkDevice
  zero-copy app) that the renderer is correct (cyan albedo, valid `(0,∓1,0)`
  world normal). The black was **light-facing**: the box's camera-visible
  vertical faces pointed away from the overhead lights. Fixed with a hero-box
  relight (two opposed fills) and the emissive crutch dropped — the box now
  shades as proper lit cyan.
- **"M7 spot shadow not visible" — root cause was the spot DIRECTION.** The
  spot emitted **zero light**: Atom's `SimpleSpotLight` emits along its local
  **+Z** (`transform.GetBasisZ()`) but the demo aimed local **-Z** at the
  target, so the cone pointed 180° away. Fixed with a code-computed look-at
  `q:(0,0,+1)→dir`; the spot now lights a clear blue cone where aimed.
- **"Single light greys the render" — DISPROVEN.** It is the intermittent
  QSG/present-race grey, independent of light count or FP registration. The
  capture harness now grey-detects (centre-crop) and retries.

## Diagnostic knobs added (env-gated, off by default)
`GZ_O3DE_RDC_FRAME=<n>` (in-app RenderDoc capture), `GZ_O3DE_SPOT_TARGET="x y z"`,
`GZ_O3DE_SPOT_INTENSITY=<cd>`, `GZ_O3DE_SUN_INTENSITY=<lux>`,
`GZ_O3DE_DEMO_NO_BOXFILL=1`, `GZ_O3DE_DEMO_NO_SUN=1`, `GZ_O3DE_DEMO_ONLY_LIGHT`,
`GZ_O3DE_MESH_EMISSIVE` (legacy crutch, now off).

## Known remaining work (post-beta2)
- **Shadow passes** are the open lighting piece: neither the spot's cast
  shadow nor directional cascade shadows are visually confirmed. The lights
  both work; the shadow rendering is unverified. Next step: a RenderDoc
  capture to confirm whether the runtime-built caster draws into the
  shadowmap at all (cooked vs runtime, echoing the runtime-mesh saga).
- Real metallic/roughness/textures from `O3deMaterial` (still an M0 stub).

See `task-tracker.md` and `HANDOFF_FOR_NEXT_AGENT.md` for full detail.
