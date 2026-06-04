# Manual visual verification — M5, M6, M7

How to run the o3de live demo and confirm visually that every milestone
since M5 is working. Run the demo once, then walk through the checklist
with the window in front of you and tick each item.

## Launch

```bash
/home/jrivero/code/gz/gz-rendering/o3de/examples/native_vulkan_live/live_demo.sh
```

It spawns a `Gazebo GUI` window (orange title bar, blue "O3DE / Atom --
native Vulkan zero-copy (live)" sub-header). First-frame appears within
~3 s; give it 10–15 s of warmup before judging — Qt's QSG node resizes
once during the bootstrap and the imported Vulkan image regenerates,
during which frames can be off. **Move the mouse over the viewport
periodically** to keep the Qt scheduler ticking.

Close with the X in the upper-right or `Ctrl+C` in the terminal.

## Scene layout (camera fixed; this is what you should see)

```
  ┌──────────────────────────────────────────────────────────┐
  │ Gazebo GUI                                               │
  ├──────────────────────────────────────────────────────────┤
  │ O3DE / Atom -- native Vulkan zero-copy (live)            │
  ├──────────────────────────────────────────────────────────┤
  │                                                          │
  │  cyan frustum                  orange wall plane         │
  │  (wireframe)        white sphere (M7 caster)             │
  │                     ╲           ╱                        │
  │                      ╲         ╱                         │
  │   ┌─red box─┐  ┌yellow cage┐                magenta      │
  │   │         │  │  green ball│               capsule      │
  │   │  blue   │  │            │              ┌──╮          │
  │   │ cyl. ━━ │  │   tiny RGB │              │  │          │
  │   └─────────┘  │   axis arr.│              │  │          │
  │  ╲╱   ╲╱  ╲╱   └─────────────┘              ╰──╯          │
  │  ─────────────────────────────────────────────────       │
  │            grey grid floor (z=0)                         │
  │                                                          │
  └──────────────────────────────────────────────────────────┘
```

## Checklist — go top-left to bottom-right

| # | Look for | Where | Milestone | What "pass" looks like |
|---|----------|-------|-----------|------------------------|
| 1 | **Orange/red box, spinning slowly** | left-of-centre, on the grid | M0 (baseline AuxGeom) | Spins about +Z, one revolution ~6 s |
| 2 | **Green sphere, bobbing up & down** | centre, on the grid | M0 | Z oscillates ~0.3 m, period ~3 s |
| 3 | **Yellow wireframe cage around the green sphere** | tracks the sphere | M0 wireframe primitive | Cage stays centred on the bobbing sphere |
| 4 | **Blue cylinder, orbiting** | sweeps a circle in front of the camera | M0 | Slow orbit, period ~9 s |
| 5 | **Magenta capsule, slowly tilting** | right side, on the grid | **M5 Phase A** | Cylinder body with rounded caps; tilts ±0.5 rad about its Y axis, period ~10 s |
| 6 | **Cyan frustum widget (wireframe pyramid)** | upper-left of viewport, above the grid | **M5 Phase D** | Hollow wireframe pyramid with diagonals to the apex |
| 7 | **Tiny RGB arrows (R=+X, G=+Y, B=+Z) at world origin** | centre, between the box and the grid | **M5 Phase C** | Three short colour-coded arrows; arrows = cylinder shaft + cone tip |
| 8 | **Orange wall plane** | far back centre, behind the row | M0 (PLANE primitive) | Solid orange rectangle standing vertical |
| 9 | **Grey grid floor (20×20 cells, 1 m each)** | covers the whole z=0 plane | M0 | Lines should be visible all the way to the horizon — no abrupt cutoff |
| 10 | **White shaded sphere "stacked" above the green one** | next to / behind the bobbing sphere | **M7 Phase B** | This is the cooked-mesh sphere (`sphere.fbx.azmodel`) — a PBR-shaded sphere, distinct from the flat-shaded AuxGeom green one. It should have visible gradient shading (lit side bright, opposite side dark). It is stationary at world (1, 0, 1) |
| 11 | **Subtle warm tint on the upward-facing surfaces** | top of cube, sphere, capsule | **M6 Phase C** point light | The point light is at (0, 0, 3) — directly above the origin, warm white. Surfaces facing up should look slightly warmer than surfaces facing the ground |
| 12 | **Subtle cool/blue tint on the right side of geometry** | facing the spot at (-2, 2, 2) | **M6 Phase C** spot light | The spot is cool blue. Look for a faint blue cast on the side of the magenta capsule and the white sphere that faces the spot |

## What is NOT visible (and why)

This is the deliberately-known gap from this session:

* **A visible cast shadow** from the white sphere caster onto a surface.
  M7 Phase A wired the `ProjectedShadowFeatureProcessor` and a per-frame
  shadow descriptor; M7 Phase B added the sphere caster mesh. But the
  receiver mesh I picked (`occlusionculling plane`) is a transparent
  occlusion-debug asset, not a solid surface — it does not visibly
  receive shadows. The orange wall behind everything is an AuxGeom
  primitive, which does not participate in the shadow pass either.
  So the shadow is being rendered into the depth atlas but has no solid
  Mesh surface to fall on.
* **A visible directional sun** (M6's `DirectionalLightFeatureProcessor`
  is intentionally disabled — enabling it currently grey-screens the
  demo; see the commit log for `dbc2d41f`).

## How to confirm engine telemetry while looking at the demo

In a second terminal, before launching:

```bash
nohup /home/jrivero/code/gz/gz-rendering/o3de/examples/native_vulkan_live/live_demo.sh \
  > /tmp/o3de_manual.log 2>&1 &
disown
sleep 12
# Then in the GUI: move the mouse over the viewport for ~10 s to let
# frames accumulate.

# Now check the telemetry:
grep -E 'FPs cached|demo mesh acquired|light acquired|shadow acquired' \
  /tmp/o3de_manual.log
grep -oE 'interop live frame [0-9]+' /tmp/o3de_manual.log | tail -1
```

What you should see in that grep (each line = one milestone proven):

```
[gz-o3de] M6 light  FPs cached: dir=(nil) point=0x... spot=0x...
[gz-o3de] M7 shadow FPs cached: projected=0x... mesh=0x...
[gz-o3de] M7 demo mesh acquired: sphere-caster @ (1.00,0.00,1.00) ...
[gz-o3de] M7 demo mesh acquired: plane-receiver @ (0.00,0.00,0.00) ...
[gz-o3de] M6 point light acquired: id=0xD0001 (total point=1)
[gz-o3de] M6 spot  light acquired: id=0xD0002 (total spot=1)
[gz-o3de] M7 spot  shadow acquired: id=0xD0002 (total spotShadow=1)
interop live frame N         (N should be in the hundreds after ~15 s)
```

And **no** lines mentioning errors against `shadow`, `mesh`, `model`,
or `asset`. (The `Default/TransRed/Green/Blue already registered` lines
are pre-existing M5 noise and can be ignored.)

If all 12 checklist items above are visible AND the telemetry block
appears clean AND frames are accumulating at hundreds-per-15s, M5
through M7 Phase B are all visually proven and the only remaining
known issue is the shadow-visibility gap documented above.
