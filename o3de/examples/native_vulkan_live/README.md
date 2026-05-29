# Live native Vulkan→Vulkan zero-copy demo

A runnable demo of the experimental **O3DE/Atom** gz-rendering backend driving
gz-gui's `MinimalScene` over **native Vulkan→Vulkan zero-copy**: Atom renders the
scene into an image on its own `VkDevice`, exports it as an FD
(`vkGetMemoryFdKHR`), and Qt's Vulkan RHI imports that image onto *its* `VkDevice`
and samples it directly. No CPU readback, no intermediate copy.

## What you should see

A grey viewport you can orbit / pan / zoom, with:

| element | colour | position |
|---------|--------|----------|
| box | red | left (`y = +1.5`) |
| sphere | green | centre (origin) |
| cylinder | blue | right (`y = -1.5`, taller) |
| ground grid | grey | 20×20 unit cells on the floor |
| wireframe cage | yellow | around the green sphere |
| plane (wall) | orange | standing behind the row |

These come from `GZ_O3DE_DEMO_SHAPES` (set by the script) — they stand in for
what a real Gazebo scene (gz-sim) would push into the engine. The box, sphere
and cylinder are AuxGeom solids; the grid, wireframe box and plane exercise the
line/quad geometry path.

## Run it

```bash
./live_demo.sh
```

Close the window (or `Ctrl-C` in the terminal) to stop. There is **no timeout** —
it runs until you close it, so you can interact with the camera.

## Prerequisites

This is an experimental PoC with a specific build/runtime setup:

1. **gz-rendering built with the interop path** — `-DGZ_O3DE_INTEROP=ON`, with the
   small O3DE gem patch in [`o3de/patches/`](../../patches/) applied to the vendored
   Atom tree, installed into a colcon workspace.
2. **The Vulkan-interop gz-gui** in that same workspace's `install/` — the script
   sets `GZ_CONFIG_PATH` to the workspace so the patched `MinimalScene` is used
   instead of a system gz-gui.
3. **A vendored O3DE build** at `vendor/o3de/build/linux/bin/profile` (the Atom
   runtime `.so`s, added to `LD_LIBRARY_PATH`).

See [`../../README.md`](../../README.md) and
[`../../M4_INTEROP_DESIGN.md`](../../M4_INTEROP_DESIGN.md) for how the workspace is
built, and [`../../docs/zero-copy-interop-findings.md`](../../docs/zero-copy-interop-findings.md)
for how the zero-copy path works (and the debugging journey behind it).

## Paths

Defaults target the reference machine; override via the environment if yours
differs:

| var | meaning | default |
|-----|---------|---------|
| `GZ_O3DE_WS` | colcon workspace (its `install/` has gz-rendering + gz-gui) | `~/code/gz/ws_o3de_rendering` |
| `GZ_O3DE_REPO` | gz-rendering checkout (for `vendor/o3de` runtime libs) | `~/code/gz/gz-rendering` |
| `GZ_O3DE_DEMO_CONFIG` | gz-gui config to load | `live_demo.config` next to the script |

## What the script sets

| env var | why |
|---------|-----|
| `GZ_GUI_RENDER_ENGINE_GUI_API_BACKEND=vulkan` | Qt MinimalScene uses the Vulkan RHI (the importer side) |
| `GZ_O3DE_INTEROP=1` | enable the interop image export at all |
| `GZ_O3DE_INTEROP_LIVE=1` | per-frame live rendering into the shared image (vs. the static probe) |
| `GZ_O3DE_INTEROP_SEM=1` | publish the render-finished **timeline semaphore** (#26) so Qt waits for the frame on its device |
| `GZ_O3DE_DEMO_SHAPES=1` | inject the box/sphere/cylinder when the scene is empty |

## Diagnostics

A couple of toggles are useful if something misbehaves (export before running):

* `GZ_O3DE_NO_RESIZE=1` — never recreate the shared image on window resize (the
  isolation experiment that pinned the original device-loss root cause).
* `GZ_O3DE_INTEROP_SEM=0` — drop the timeline semaphore (host-sync only) to
  compare.
* `GZ_O3DE_DUMP_PNG=1` — once the scene settles, read the imported image back on
  Qt's device and write it to `/tmp/o3de_consumer.ppm` (override with
  `GZ_O3DE_DUMP_PATH`). The supported way to inspect what the engine renders
  without screen-grabbing the display; convert with e.g.
  `convert /tmp/o3de_consumer.ppm out.png`.

Per-frame interop logging (producer + consumer) is printed periodically — run
with `-v 4` (edit the `exec` line) for the full firehose.
