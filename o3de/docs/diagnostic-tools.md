# O3DE backend — diagnostic tools

This document catalogues the in-tree diagnostic tools shipped with the O3DE/Atom
backend: every `GZ_O3DE_*` environment variable, the helpers they invoke, and
the bug class each one is designed to surface. They were grown organically
while bringing up the native Vulkan→Vulkan zero-copy display path; the goal of
this file is to make them discoverable and reproducible later.

> The interop tools are all gated behind `GZ_O3DE_INTEROP=1` and only have
> effect when the backend was built with `-DGZ_O3DE_INTEROP=ON` (the default,
> requires the O3DE gem patch in `o3de/patches/`). In a patch-free build they
> are logged no-ops.

## Cheat sheet

| Env var | Default | One-line purpose |
|---------|---------|------------------|
| `GZ_O3DE_DEMO_SHAPES` | off | Inject a red box / green sphere / blue cylinder when the scene is empty |
| `GZ_O3DE_SSAA` | 2 | Supersampling factor for anti-aliasing (1=off, up to 4) |
| `GZ_O3DE_DUMP_FRAME` | off | Dump the first rendered frame from the CPU-readback path to `/tmp/gz_gui_frame.ppm` |
| `GZ_O3DE_INTEROP` | off | Enable the M4 exportable-image FD path (foundation for zero-copy display) |
| `GZ_O3DE_INTEROP_LIVE` | off | Render the live scene into the export image every frame (Phase 2 of M4) |
| `GZ_O3DE_INTEROP_SEM` | off | Advertise + wait on the render-finished timeline semaphore (per-frame cross-device sync) |
| `GZ_O3DE_INTEROP_GLTEST` | off | Headless Vulkan→GL import self-test (PASS/FAIL log) |
| `GZ_O3DE_INTEROP_GLDUMP` | unset | Path to dump the GL-imported image as PPM (only with `GLTEST`) |
| `GZ_O3DE_INTEROP_VKTEST` | off | Headless Vulkan→Vulkan import self-test + sampler check (PASS/FAIL log) |
| `GZ_O3DE_INTEROP_VKDUMP` | unset | Path to dump the Vk-imported image as PPM (only with `VKTEST`) |
| `GZ_O3DE_DUMP_PNG` | off | On the consumer device, transfer-copy the imported image to a PPM (frame 30) |
| `GZ_O3DE_DUMP_PATH` | `/tmp/o3de_consumer.ppm` | Path for `GZ_O3DE_DUMP_PNG` |
| `GZ_O3DE_SAMPLE_PROBE` | off | On the consumer device, *sampler*-path readback to a PPM (frame 31) |
| `GZ_O3DE_SAMPLE_PROBE_PATH` | `/tmp/o3de_sample_probe.ppm` | Path for `GZ_O3DE_SAMPLE_PROBE` |
| `GZ_O3DE_DIAG_CLEAR` | off | Overwrite the imported image with solid red on the consumer device every frame |
| `GZ_O3DE_PRODUCER_LAYOUT` | 0 (SHADER_READ) | Sweep the consumer acquire's `oldLayout` (0/1/2/3) |
| `GZ_O3DE_NO_RESIZE` | off | Keep the live path rendering at the first created size (never recreate) |
| `GZ_O3DE_NO_QFOT` | off | Consumer acquire uses a plain layout barrier instead of QFOT |
| `GZ_O3DE_NO_IDLE_TICK` | off | Render thread does not tick Atom while idle on the live path |

The remaining `GZ_O3DE_*` variables — `GZ_O3DE_ENGINE_PATH`, `GZ_O3DE_PROJECT_PATH`,
`GZ_O3DE_PROJECT_NAME`, `GZ_O3DE_BIN_PATH` — are not diagnostics; they configure
runtime paths and are documented in [`../README.md`](../README.md).

## Self-tests: `GZ_O3DE_INTEROP_GLTEST` and `GZ_O3DE_INTEROP_VKTEST`

These are the **canonical regression tests for the working zero-copy paths**.
They run during `O3deBackend::Run()` once the interop image is ready and log a
PASS/FAIL summary; no gz-gui or window needed.

### `GZ_O3DE_INTEROP_VKTEST=1` — Vulkan→Vulkan self-test (`o3de/src/O3deVkInterop.cc`)

Headless Vulkan→Vulkan stand-in for Qt's QRhi device. Stands up its own private
`VkInstance`+`VkDevice` on the same physical GPU, imports the exported `OPAQUE_FD`
through the **same** shared helper (`O3deVkImportImage` in `O3deVkImport.cc`)
the production gz-gui consumer uses, and runs **two** independent readbacks of
the producer's gradient:

1. **Transfer-copy readback** (`vkCmdCopyImageToBuffer`) — reads memory.
   Compares each texel to the deterministic gradient
   `R=x, G=y, B=128, A=255`. Logs `[gz-o3de] vktest: PASS/FAIL`.
2. **Sampler-path readback** (`O3deVkSampleProbeRgba`) — reads through a real
   `VkSampler` using `texelFetch` in a compute shader (the same access path
   `QSGSimpleTextureNode` uses). Compares byte-for-byte to the transfer-copy
   result. Logs `[gz-o3de] vksamplertest: PASS/FAIL`.

A PASS on **both** locks in three findings the bring-up debugging produced:

* The producer's `vkGetMemoryFdKHR` export is well-formed (the FD round-trips).
* The cross-device OPAQUE_FD memory import is correct
  (`VkMemoryDedicatedAllocateInfo` mirrors the producer's dedicated allocation —
  see hypotheses 12/13/15 in [`zero-copy-interop-findings.md`](zero-copy-interop-findings.md)).
* The consumer device's *sampler* reads the producer's pixels correctly — NOT
  just the transfer copy. This was the empirical question that motivated
  splitting the test in two: a sampler-vs-transfer divergence would point at a
  cross-device tiling-swizzle bug, while a divergence at the Qt level (PASS
  here, grey on screen) isolates the bug to gz-gui's QRhi/QML draw of the
  `QSGVulkanTexture::fromNative` wrapper.

The test is invoked once from `O3deBackend.cc:646` and protects against
producer/consumer FD-export regressions for free. To run it:

```bash
GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_VKTEST=1 \
  gz gui -c examples/config/scene3d.config 2>&1 | grep '\[gz-o3de\] vk'
# Expected: vktest: PASS and vksamplertest: PASS.
```

Optionally dump the imported gradient to a PPM with `GZ_O3DE_INTEROP_VKDUMP=/tmp/x.ppm`.

### `GZ_O3DE_INTEROP_GLTEST=1` — Vulkan→GL self-test (`o3de/src/O3deGlInterop.cc`)

The OpenGL-side sibling: imports the exported FD into a GL texture via
`GL_EXT_memory_object_fd` on a private EGL context and compares the texels.
Used during the original Vulkan→GL display milestone; kept as a regression
check for the GL import path. Logs `[gz-o3de] gltest: PASS/FAIL`. Dump path:
`GZ_O3DE_INTEROP_GLDUMP`.

## Consumer-side probes (run in `O3deCamera.cc`, in the gz-gui process)

These three fire on the live consumer side (Qt's `VkDevice`) and write PPMs
that can be inspected without a window grab. They are the ones used to
disambiguate `QSGSimpleTextureNode`-renders-uniform-grey from
cross-device-sampler-is-broken.

### `GZ_O3DE_DUMP_PNG=1` — transfer-copy readback to PPM (frame 30)

Calls `O3deVkReadbackImageRgba` (see `O3deVkImport.hh:102`) — transitions the
imported image to `TRANSFER_SRC`, `vkCmdCopyImageToBuffer` to a host-visible
buffer, writes a PPM. Reads memory, not through a sampler.

What it diagnoses: "is the producer's pixel content in shared memory at all,
on the consumer device?" PASS = a PPM showing the producer's rendered shapes.

```bash
GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_LIVE=1 GZ_O3DE_INTEROP_SEM=1 \
  GZ_O3DE_DUMP_PNG=1 GZ_O3DE_DUMP_PATH=/tmp/consumer.ppm \
  gz gui -c <config>
# Open /tmp/consumer.ppm: shapes = memory OK; uniform = producer export broken.
```

### `GZ_O3DE_SAMPLE_PROBE=1` — sampler-path readback to PPM (frame 31)

Calls `O3deVkSampleProbeRgba` (see `O3deVkImport.hh:116`). Same image, but
read through a real `VkSampler` via `texelFetch` in a compute shader
(`shaders/o3de_sample_probe.comp`, embedded as `O3deSampleProbeSpv.h`). Reads
through the **same access path** Qt uses for `QSGSimpleTextureNode`.

What it diagnoses: "does the consumer device's sampler read the producer's
pixels correctly?" Compared against `GZ_O3DE_DUMP_PNG`:

| `DUMP_PNG` (transfer) | `SAMPLE_PROBE` (sampler) | Conclusion |
|-----------------------|-------------------------|------------|
| shapes | shapes | Cross-device interop **and** sampler are correct; bug is in Qt/QRhi draw of `fromNative` texture. |
| shapes | uniform | Cross-device **sampler** is broken (tiling swizzle, missing dedicated alloc, format mismatch). |
| uniform | uniform | Cross-device **memory sharing** is broken (FD export, layout, sync). |

The current state of the live demo is the first row: both PPMs show the
producer's shapes, yet the gz-gui window stays uniform grey — see
[`zero-copy-interop-findings.md`](zero-copy-interop-findings.md) for the
follow-up debugging targeting `QSGSimpleTextureNode`/`fromNative`.

```bash
# Capture both PPMs in one run, then compare visually:
GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_LIVE=1 GZ_O3DE_INTEROP_SEM=1 \
  GZ_O3DE_DUMP_PNG=1 GZ_O3DE_DUMP_PATH=/tmp/transfer.ppm \
  GZ_O3DE_SAMPLE_PROBE=1 GZ_O3DE_SAMPLE_PROBE_PATH=/tmp/sampler.ppm \
  gz gui -c <config>
```

### `GZ_O3DE_DIAG_CLEAR=1` — overwrite the imported image with solid red each frame

Calls `O3deVkClearImageDiag` (see `O3deVkImport.hh:107`) on Qt's own
`VkDevice`. The cleanest test of "is the gz-gui window sampling **this** image
at all?": if the window turns red, yes; if it stays grey, Qt is showing
something else (a placeholder texture, a wrong subrect, a stale framebuffer).

```bash
GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_LIVE=1 GZ_O3DE_DIAG_CLEAR=1 gz gui -c <config>
# Expected (everything OK): window turns red. Observed today: window stays grey
# -> confirms the bug is in the QSGSimpleTextureNode/fromNative draw, not in
# the cross-device interop.
```

## `GZ_O3DE_PRODUCER_LAYOUT` — layout sweep

Sets the consumer acquire's `oldLayout` to one of four candidates
(`O3deCamera.cc:243`):

| Value | `oldLayout` | When you'd use it |
|-------|-------------|-------------------|
| 0 (default) | `SHADER_READ_ONLY_OPTIMAL` | The layout Atom's last pass leaves the image in for the current pipeline. |
| 1 | `TRANSFER_SRC_OPTIMAL` | After a `UseCopyAttachment Read` (the static-probe scope). |
| 2 | `GENERAL` | Diagnostic only; "trust the driver to figure it out". |
| 3 | `COLOR_ATTACHMENT_OPTIMAL` | If the producer truly leaves it as a colour attachment. |

A wrong `oldLayout` on NVIDIA can mean the importing device's sampler reads
the image's *unresolved* (compressed/cleared) state. The sweep was used to
verify Atom's real final layout is `SHADER_READ`. The 4-way sweep produced
byte-identical results in the recent investigation, isolating the bug elsewhere.

## "Disable a feature" toggles (negative diagnostics)

These three turn pieces of the path **off** to isolate a hypothesis. Each one
disproved one earlier theory (see the elimination table in
[`zero-copy-interop-findings.md`](zero-copy-interop-findings.md)):

* **`GZ_O3DE_NO_RESIZE=1`** — keep rendering at the first created size; never
  recreate the exportable image (`O3deBackend.cc:1871`). The **decisive
  experiment** that isolated the consumer-side resize/re-import use-after-free
  as the device-loss cause. Also useful as a stress toggle to rule out resize
  churn during unrelated investigation.
* **`GZ_O3DE_NO_QFOT=1`** — consumer acquire uses a plain layout barrier
  instead of the EXTERNAL queue-family ownership transfer (`O3deVkImport.cc:248`).
  Used to rule out QFOT as the device-loss trigger.
* **`GZ_O3DE_NO_IDLE_TICK=1`** — the render thread does not tick Atom while
  the live path is idle (`O3deBackend.cc:1516`). Used to rule out idle ticks
  racing Qt's sampling.

## C++ helpers behind the env vars

If you want to invoke these checks from C++ (e.g. from a test), they live in
[`o3de/src/O3deVkImport.hh`](../src/O3deVkImport.hh):

| Function | Used by | Purpose |
|----------|---------|---------|
| `O3deVkImportImage` | every consumer | Import an OPAQUE_FD image + (optional) timeline semaphore |
| `O3deVkAcquireFromProducer` | every consumer | Queue-family acquire + layout transition + timeline-semaphore wait |
| `O3deVkDestroyImported` | every consumer | Free imported handles, close FDs |
| `O3deVkReadbackImageRgba` | `GZ_O3DE_DUMP_PNG`, `VKTEST` | Transfer-copy → host buffer (memory readback) |
| `O3deVkSampleProbeRgba` | `GZ_O3DE_SAMPLE_PROBE`, `VKTEST` | `texelFetch` through a sampler → host buffer (sampler readback) |
| `O3deVkClearImageDiag` | `GZ_O3DE_DIAG_CLEAR` | Solid-colour clear (visibility probe) |

## The sampler-probe compute shader

The shader source for `O3deVkSampleProbeRgba` lives in
[`o3de/src/shaders/o3de_sample_probe.comp`](../src/shaders/o3de_sample_probe.comp);
its pre-compiled SPIR-V is embedded as a `static const uint32_t[]` in
[`o3de/src/O3deSampleProbeSpv.h`](../src/O3deSampleProbeSpv.h) so the backend
ships no shader-compiler dependency at build time.

To regenerate the embedded SPV after editing the shader:

```bash
glslc -O -mfmt=c o3de/src/shaders/o3de_sample_probe.comp \
  -o /tmp/o3de_sample_probe.spv.inc
# then paste the contents of /tmp/o3de_sample_probe.spv.inc into the
# initializer list of kO3deSampleProbeSpv[] in O3deSampleProbeSpv.h.
```

Byte-identity check (the source must round-trip through `glslc -O -mfmt=c`
to the same SPIR-V word stream as the embedded header — invariant verified
on regeneration):

```bash
glslc -O -mfmt=c o3de/src/shaders/o3de_sample_probe.comp -o /tmp/fresh.inc
diff <(grep -oE '0x[0-9a-fA-F]+' /tmp/fresh.inc) \
     <(grep -oE '0x[0-9a-fA-F]+' o3de/src/O3deSampleProbeSpv.h)
# Empty output = identical.
```

## See also

* [`zero-copy-interop-findings.md`](zero-copy-interop-findings.md) — the full
  root-cause analysis and elimination table that motivated each tool.
* [`task-tracker.md`](task-tracker.md) — task-ID provenance (#20–#26).
* [`../README.md`](../README.md) — backend overview and runtime path knobs.
