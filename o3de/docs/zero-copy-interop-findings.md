# O3DE↔Qt native Vulkan zero-copy interop — root-cause findings & resolution

Status: **WORKING** for stability + cross-device pixel transport (~2000 frames at
~60-100 fps across multiple window resizes, zero device losses). Two fixes landed
the stability: (1) the #26 render-finished timeline semaphore, and (2) the consumer
**retiring** old imports instead of freeing them under Qt's in-flight frame.
**Remaining issue (open, 2026-05-30):** a separate gz-gui-side rendering bug -- the
QSGSimpleTextureNode draw of the cross-device fromNative-wrapped image renders
uniform on screen even though the imported VkImage carries the correct pixels (a
consumer-side compute sampler probe confirms it). See the new "Remaining issue:
QSGSimpleTextureNode renders uniform" section below. Date: 2026-05-30.

This documents the full debugging journey — including **two wrong root-cause
conclusions** that the evidence later overturned — so the reasoning is transparent:
(a) the #26 implementation + evidence it works, (b) the real root cause (a consumer
resize/re-import use-after-free) and the fix, (c) every hypothesis tested and the
evidence that eliminated it.

> **Honesty note on the journey.** This file twice concluded the wrong root cause:
> first "missing render-finished semaphore" (disproven once #26 worked and the loss
> persisted), then "cross-device render-target/compression handoff" (disproven by the
> `GZ_O3DE_NO_RESIZE` experiment below: a single fixed-size live image samples cleanly
> for 2000+ frames). The actual cause was mundane: the consumer freed an imported
> VkImage while Qt still had an in-flight frame sampling it. Both earlier "fixes" (the
> semaphore; the layout/QFOT experiments) were still useful — the semaphore is real
> required infrastructure — but neither was the device-loss trigger.

> **Final visual confirmation (2026-05-29).** The earlier "stable" claim was based on
> frame counts + zero device loss; the *scene content* was later confirmed directly.
> Reading the imported image back on Qt's own `VkDevice` yields pixels identical to
> the producer's pipeline output (the demo box/sphere/cylinder), and the on-screen
> result matches — so the cross-device colour-attachment handoff carries full detail,
> not just the background (a worry the disproven "compression handoff" theory had
> raised). The one remaining gotcha was mundane and *not* a rendering bug: the demo's
> initial camera framed the shapes small and low under a large grey sky, which read as
> "all grey" until the camera was reframed. A runnable demo lives at
> [`o3de/examples/native_vulkan_live/`](../examples/native_vulkan_live/).

## Goal

Display O3DE/Atom's live offscreen render in gz-gui's MinimalScene via **native
Vulkan→Vulkan zero-copy**: Atom (its own `VkDevice`, "device A") renders into an
exportable colour image; gz-gui/Qt (a separate `VkDevice`, "device B") imports that
image (`OPAQUE_FD`) and samples it with no CPU readback. This is milestone **M4**
(post-PoC, optional); the CPU-readback path is the plan's primary display path and
already works.

Two phases:
- **Phase 1 — static probe (DONE, stable):** Atom uploads a gradient into the export
  image *once* (`UpdateImageContents`); Qt imports + samples it. Verified displaying.
- **Phase 2 — live (#25 + #26): WORKING.** Atom renders the scene into the export image
  *every frame* (`CreateRenderPipelineForImage`); Qt samples it zero-copy. This is where
  the device loss used to occur — now fixed (see below).

## #26 IMPLEMENTED — and proven working

The render-finished timeline semaphore is now built end-to-end and **verified**:

**Producer** (`O3deBackend.cc`): a render-finished **timeline** fence
(`RHI::Fence::Init(..., usedForWaitingOnDevice=true)` → `TimelineSemaphoreFence`;
the default `false` makes a *binary* fence, which has no exportable `VkSemaphore`
and asserts in `GetFenceNativeHandle` — that was a real groundwork bug). A standalone
`RHI::ScopeProducerFunctionNoData` (`EnsureFenceSignalScope`) copy-reads the interop
image and calls `FrameGraphInterface::SignalFence`; it is imported each rendered frame
via `RHISystemNotificationBus::Handler::OnFramePrepare` →
`FrameGraphBuilder::ImportScopeProducer`, gated to one tick/frame by
`signalFenceThisTick`. Per frame: read `GetFencePendingValue`, publish it, `Reset()`
to advance the value. No custom `RPI::Pass` subclass was needed (the route that had
looked blocked).

**Consumer** (`O3deVkImport.cc` / `O3deCamera.cc`): imports the semaphore as a
**timeline** type (`VkSemaphoreTypeCreateInfo`, permanent import) and waits on the
per-frame value via `VkTimelineSemaphoreSubmitInfo` in the acquire submit.

**Evidence it works** (`GZ_O3DE_INTEROP_SEM=1`):
```
[gz-o3de] interop live frame 1: ... fence=Signaled value=1
[gz-o3de] interop live frame 2: ... fence=Signaled value=2     (producer signals, monotonic)
[gz-o3de] vkimport: acquire wait=0 shared-counter=1 submit=0
[gz-o3de] vkimport: acquire wait=1 shared-counter=2 submit=0   (counter shared across devices; wait<=counter)
```
The shared timeline counter advances **across the two `VkDevice`s** in lockstep with
the producer, and every consumer wait value is ≤ the counter (so the wait is satisfiable
and cannot hang). Cross-device timeline-semaphore sharing is therefore fully functional.

## Actual root cause (confirmed by fix) — consumer resize/re-import use-after-free

The device loss is a **use-after-free across the producer/consumer device boundary**,
triggered by an image **resize**:

1. The producer creates the exportable image at a placeholder size in setup
   (gen=1), then recreates it at the real camera/window size on the first frame
   (gen=2) — and again on any window resize (gen=3, …). Each recreate bumps the
   generation and exports a fresh FD.
2. On a generation change the consumer (`O3deCamera::RenderTextureMetalId`) re-imports
   the new image — and, in the original code, **immediately destroyed the old import's
   VkImage**.
3. But Qt's scene graph still had an **in-flight frame sampling the old VkImage**.
   Destroying it out from under that frame faulted **Qt's** device — which is why only
   Qt's device died (producer stayed healthy, `waitIdle=VK_SUCCESS`) and the consumer's
   *next* acquire `vkQueueSubmit` returned `VK_ERROR_DEVICE_LOST` in 0 ms (already
   dead, not a hung wait).

### The decisive experiment (`GZ_O3DE_NO_RESIZE`)
Forcing the live path to keep rendering at the already-created image size (never
recreate → consumer imports exactly once) ran **2000 frames with zero device loss**.
That single experiment overturned the compression theory (the live, compressed,
colour-attachment image samples cleanly across devices) and pinned the cause to the
resize / re-import churn.

### Why the static probe worked all along
The static probe creates its image once and never recreates it, so the consumer
imports once and never re-imports — it simply never hit the use-after-free. (It is also
written by a plain `UpdateImageContents`, which is why the compression theory looked
plausible — but compression was a red herring; a *live, compressed* image at a fixed
size samples fine, per the `NO_RESIZE` run.)

## The fix

1. **Consumer retires old imports** (`O3deCamera`): on a re-import, push the old
   `O3deVkImportedImage` onto a `retiredImports` list instead of destroying it; free
   the whole list only when the camera is destroyed. Qt's in-flight frame keeps a live
   VkImage. (Resizes are rare, so the retained memory is bounded and cheap.)
2. **#26 render-finished timeline semaphore** (above): the producer signals it after
   each frame's render into the shared image; the consumer waits on the per-frame value
   before sampling — the correct cross-device "frame ready" edge.
3. **Image created in setup, not deferred:** the exportable image must exist before the
   consumer builds its Qt texture node (a deferred-creation attempt crashed in
   `QRhi::endFrame` because Qt presented a null VkImage). The first real frame still
   recreates it at the camera size — now survivable thanks to fix 1 — and settles the
   new pipeline's MSAA shader variants before the consumer samples it.

**Result:** ~2000 frames, ~60-100 fps, multiple resizes (gen 1→2→3), the #26 shared
timeline counter advancing in lockstep every frame, zero device loss / crash.

## Hypotheses tested and eliminated (systematic debugging)

Earlier session (still valid):

| # | Hypothesis | Test | Result |
|---|-----------|------|--------|
| 1 | `MainPipeline` forced single-sample → deferred G-buffer shaders need MSAA | 4× MSAA on the live `CreateRenderPipelineForImage` descriptor + `SetApplicationMultisampleState` | ✅ **Real bug, FIXED.** SRG "multisample count 1" error flood → **0**. Device loss persisted → separate issue. |
| 2 | `GzAtomPoc` probe (2nd scene/pipeline/capture on the shared device) | Env-gate the probe off under `GZ_O3DE_INTEROP` | ✅ Ruled out — loss unchanged. |
| 3 | Unmatched `VK_QUEUE_FAMILY_EXTERNAL` ownership acquire | `GZ_O3DE_NO_QFOT=1` → plain `IGNORED` layout barrier | ✅ Ruled out — loss unchanged. |
| 4 | Producer-side GPU fault | Log `vkDeviceWaitIdle` return per frame | ✅ Ruled out — `waitIdle=VK_SUCCESS` every frame. |
| 5 | The ~5 s == our consumer `wait_for(5s)` timeout | Log on timeout / every frame START | ✅ Ruled out — no `TIMED OUT`; consumer just goes idle after 3 frames. |
| 6 | Atom idle ticks race Qt's sampling | `GZ_O3DE_NO_IDLE_TICK=1` | ✅ Ruled out — loss unchanged. |
| 7 | A single-device Vulkan sync bug | Force `SYNCHRONIZATION_VALIDATION` | ✅ Ruled out — zero findings name our image (validation cannot see cross-device hazards). |

This session (the decisive ones):

| # | Hypothesis | Test | Result |
|---|-----------|------|--------|
| 8 | Missing render-finished semaphore (the *previous* "root cause") | Implement #26 (timeline signal + consumer timeline wait); verify the shared counter | ✅ **Disproven as the cause.** Semaphore proven working (shared counter advances 0,1,2; waits satisfiable) — yet the device loss persists unchanged. |
| 9 | Consumer layout transition (`TRANSFER_SRC→` or `COLOR_ATTACHMENT→` `SHADER_READ`) on an image whose Qt-side layout doesn't match | Switch the consumer to a no-op `SHADER_READ→SHADER_READ` barrier (exactly the proven static-probe acquire) | ✅ Ruled out — both the real transition and the no-op barrier fail identically. |
| 10 | EXTERNAL queue-family acquire, in combination with the semaphore | `GZ_O3DE_NO_QFOT=1` **+** `GZ_O3DE_INTEROP_SEM=1` (never tested together before) | ✅ Ruled out — fails identically. |
| 11 | Consumer submit hangs (sync/TDR) vs. fails immediately (invalid op) | Time the submit + fence wait | ✅ **Decisive (mechanism):** `submit=0 ms`, `VkResult=-4`. Immediate failure → device already dead → not our wait. |
| 12 | Cross-device sampling of a **compressed colour-attachment render target** is itself the problem | Hypothesised; tested by #13 | ❌ **Disproven** by #13 — a live compressed image at a fixed size samples fine for 2000+ frames. Compression was a red herring. |
| 13 | The **resize / re-import churn** is the cause (not the live render) | `GZ_O3DE_NO_RESIZE=1` — keep rendering at the created size, never recreate, consumer imports once | ✅ **DECISIVE:** 2000 frames, zero device loss. Isolates the cause to resize/re-import (which was confounded with "live render" because every failing run resized 512→window on frame 1). |
| 14 | Just create the image lazily at the live size (avoid the resize) | Defer image+pipeline creation to the first frame | ❌ Wrong fix — Qt builds its texture node before the first frame and presents a **null VkImage** → SIGSEGV in `QRhi::endFrame`. The image must exist in setup. |
| 15 | The re-import **frees the old VkImage under Qt's in-flight frame** (use-after-free) | Consumer **retires** old imports (keep alive, free at camera destruction) instead of freeing on re-import | ✅ **FIX.** ~2000 frames across gen 1→2→3 resizes, zero device loss. |

Net: the device loss was a consumer-side use-after-free on resize (hypothesis 15);
the #26 semaphore (8) is required infrastructure but was never the trigger; compression
(12) was a red herring disproven by the `NO_RESIZE` experiment (13).

## Resolution (landed)

The live zero-copy path works (see "The fix" above): consumer retires old imports +
#26 timeline semaphore + image created in setup (recreated/settled on resize). Verified
~2000 frames across multiple resizes with zero device loss.

Possible future hardening (not required for the working path):
- **Reverse edge / double-buffering:** today a single shared image is reused; the
  producer host-syncs and the consumer waits on the render-finished semaphore, which
  serialises in practice. A fully pipelined design would add a consumer→producer
  "done sampling" edge or ping-pong buffers so the producer never overwrites a frame
  Qt is still reading without a host-sync.
- **Bounded retired-import reclaim:** free retired consumer imports a few frames after
  the resize (Qt is surely done) rather than holding them until camera destruction.

## #26 — as implemented (reference)

Producer (`O3deBackend.cc`):
- `EnsureInteropSemaphore()` — `Fence::Init(AllDevices, Reset, usedForWaitingOnDevice=true)`
  (→ `TimelineSemaphoreFence`); export `GetFenceNativeHandle(...)` via `vkGetSemaphoreFdKHR`
  OPAQUE_FD.
- `EnsureFenceSignalScope()` — `ScopeProducerFunctionNoData(scopeId, FenceSignalPrepare, {}, {})`.
- `FenceSignalPrepare()` — `frameGraph.UseCopyAttachment({interopImage->GetAttachmentId()}, Read)`
  (read-after-write ordering against the pipeline; also keeps the scope from being culled)
  then `frameGraph.SignalFence(*renderFinishedFence)`.
- `OnFramePrepare(FrameGraphBuilder&)` — imports the scope when `signalFenceThisTick`.
- `RenderOneFrame()` live branch — set `signalFenceThisTick` on the last render tick;
  after host-sync read `GetFencePendingValue` + `GetFenceState`, publish the value,
  `Reset()`.
- `GetInteropImport()` advertises `semaphoreFd` only when `interopSemaphoreReady`
  (`GZ_O3DE_INTEROP_SEM`).

Consumer (`O3deVkImport.cc` / `O3deCamera.cc`):
- Import as `VK_SEMAPHORE_TYPE_TIMELINE`, permanent (`flags=0`).
- `O3deVkAcquireFromProducer(..., _waitValue)` adds `VkTimelineSemaphoreSubmitInfo`.
- `semaphoreWaitValue` plumbed `GetInteropImport` → `O3deCameraInterop` → acquire.

Note: copy-read leaves the producer image in `TRANSFER_SRC_OPTIMAL` (Atom maps
copy-read → that layout, `Conversion.cpp:469-472`); the consumer's no-op barrier and
the eventual plain-handoff make this moot.

## Diagnostic toggles left in the tree

- `GZ_O3DE_INTEROP_SEM=1` — advertise + wait on the render-finished timeline semaphore
  (off → producer still signals + logs, consumer does not wait).
- `GZ_O3DE_NO_RESIZE=1` — keep the live path rendering at the first created size, never
  recreating the exportable image (`O3deBackend.cc::RenderOneFrame`). The experiment
  that isolated the resize/re-import as the device-loss cause; also a handy stress
  toggle.
- `GZ_O3DE_NO_QFOT=1` — consumer acquire uses a plain layout barrier instead of the
  EXTERNAL queue-family ownership transfer (`O3deVkImport.cc`).
- `GZ_O3DE_NO_IDLE_TICK=1` — render thread does not tick Atom while idle on the live
  path (`O3deBackend.cc::RenderThreadMain`).
- Per-frame `interop live frame N … fence=… value=…` (producer) and
  `vkimport: acquire wait=… shared-counter=… submit=…` (consumer) logging.
- Validation: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` +
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
  (layer at `vendor/o3de/build/linux/bin/profile/`).

## Fallback (still available)

The CPU-readback path (`O3deRenderTarget::Copy()` / `AttachmentReadback` → Qt
`glTexSubImage2D`) remains the default when the patched gz-gui / Vulkan GUI backend /
`GZ_O3DE_INTEROP*` are not in play. The native zero-copy path above is now the faster
alternative when they are.

## Remaining issue: QSGSimpleTextureNode renders uniform on screen (2026-05-30)

Above proves that the cross-device interop *transports the producer's pixels intact*.
A separate symptom is still open: the gz-gui window itself shows a uniform colour
instead of the producer's rendered content, even though every probe on Qt's `VkDevice`
reads the imported image's real pixels.

### How the cross-device sampling was proven correct

Two independent readbacks on the consumer (Qt's) `VkDevice`, both gated by env vars
documented in [`diagnostic-tools.md`](diagnostic-tools.md):

1. **`GZ_O3DE_DUMP_PNG`** -- `O3deVkReadbackImageRgba` does a transfer-copy of the
   imported image to a host-visible buffer. Reads *memory*.
2. **`GZ_O3DE_SAMPLE_PROBE`** -- `O3deVkSampleProbeRgba` runs a compute shader on
   Qt's device that `texelFetch`s through a real `VkSampler` -- the *same access
   path* `QSGSimpleTextureNode` uses -- and writes RGBA8 to a storage buffer.

When both PPMs are diffed: byte-identical, showing the producer's rendered shapes.
The `vksamplertest` self-test (`GZ_O3DE_INTEROP_VKTEST`, see
[`diagnostic-tools.md`](diagnostic-tools.md)) automates this assertion -- a regression
in either the FD export, the dedicated import, or the cross-device sampler would
surface there. The cross-device sampler is provably correct.

### Hypotheses already eliminated by the new gz-gui isolation tests

A focused regression test in gz-gui isolates the bug step-by-step on Qt's own device,
removing variables one at a time:
`gz-gui/test/regression/qsg_simple_texture_node_vulkan.cc`.

| # | Hypothesis | Test | Result |
|---|-----------|------|--------|
| 16 | `fromNative` + `QSGSimpleTextureNode` is broken at the simplest level | `FromNativeRendersLinearPattern`: LINEAR + host-visible VkImage on Qt's own device, four-quadrant pattern, wrap + grab + assert | ❌ **Disproven** -- PASSES; the simple case renders the pattern correctly. |
| 17 | OPTIMAL tiling + `COLOR_ATTACHMENT \| SAMPLED \| TRANSFER_DST` usage breaks the fromNative draw on a single device | `FromNativeRendersOptimalPattern`: same pattern, OPTIMAL VkImage on Qt's own device, staging-uploaded via `vkCmdCopyBufferToImage`, transitioned to `SHADER_READ_ONLY_OPTIMAL` | ❌ **Disproven** -- PASSES; the production-usage flags on a single device render correctly. |

Net: the bug requires the **cross-device FD-imported VkImage** specifically. Not
OPTIMAL alone, not COLOR_ATTACHMENT alone, not fromNative alone.

### Hypothesis 18 -- also eliminated (cross-device FD import is fine too)

| # | Hypothesis | Test (gz-gui) | Result |
|---|-----------|---------------|--------|
| 18 | `QSGSimpleTextureNode`'s draw mishandles a `fromNative`-wrapped VkImage when the underlying VkDeviceMemory was imported from a foreign device's exported FD | `FromNativeRendersImportedFdPattern`: stand up a private VkInstance/VkDevice in the test, create an exportable OPTIMAL VkImage there with dedicated allocation, populate it, transition+QFOT-release to EXTERNAL, export the FD, import onto Qt's device with `VkMemoryDedicatedAllocateInfo`, QFOT-acquire onto Qt's queue, wrap + grab + assert (the same pattern check as the two passing tests) | ❌ **Disproven** -- PASSES. The cross-device round-trip is fine, even with dedicated allocation, QFOT, OPAQUE_FD, and OPTIMAL tiling. |

### Where the production bug must therefore live

Hypotheses 16, 17 and 18 together eliminate every Qt + Vulkan primitive variant
along the path. The closest test-level analog to the production setup -- second
VkInstance, OPTIMAL exportable image, dedicated allocation, OPAQUE_FD export +
import, QFOT release+acquire, `fromNative` + `QSGSimpleTextureNode` -- renders
the pattern correctly. The bug must therefore live in one of the dimensions
the regression test does NOT exercise:

1. **What Atom's pipeline leaves in the image vs a clean staging upload.** Atom
   renders into the exportable image via its FrameGraph (`CreateRenderPipeline
   ForImage`) -- the image is a colour attachment, not a staging-copy target.
   The "compression handoff" theory was disproven by `GZ_O3DE_NO_RESIZE`
   (hypothesis 13) and by the sampler probe (which reads the producer's pixels
   correctly), but there may be a more specific Atom-side state -- e.g. an
   incomplete copy-attachment-read in `FenceSignalPrepare` (#26's signal scope),
   a mismatched final layout the consumer's acquire doesn't undo, or a queue
   submit timing window between Atom's host-sync and Qt's frame.
2. **MinimalScene's threading model.** Production runs Atom on its own
   dedicated thread, the gz-rendering camera on the gz-gui render thread, and
   `TextureNodeRhiVulkan::CreateTexture` / `PrepareNode` on Qt's scene-graph
   thread. The regression test bypasses all of this with a single QQuickItem
   doing direct `updatePaintNode`. A handoff race between threads, or a stale
   VkImage handle being read through `camera->RenderTextureMetalId()` after a
   resize re-import, would surface in production but not in the test.
3. **Re-import-on-resize churn.** Production re-imports the image on every
   gen change; the consumer "retires" old imports past Qt's in-flight frame
   but the `QSGSimpleTextureNode`'s previously-wrapped texture may still
   reference the *old* VkImage on the next `PrepareNode`, even though the
   handle has been retired. The test creates exactly one VkImage and never
   resizes, so this is not exercised.

### Next focused diagnostic

Run the live demo with the existing diagnostics layered:
* `GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_LIVE=1 GZ_O3DE_INTEROP_SEM=1` -- normal
  live path.
* `GZ_O3DE_NO_RESIZE=1` -- pin the image so resize churn is eliminated.
* `GZ_O3DE_DUMP_PNG=1` + `GZ_O3DE_SAMPLE_PROBE=1` -- confirm the consumer
  still sees correct pixels at the moment of grey display.
* `GZ_GUI_VULKAN_DIAG=1` -- log every `VkImage` handle Qt actually wraps via
  `fromNative` and compare against the producer's reported handle. A mismatch
  here pinpoints the production-only stale-handle case.

### Findings from the next-diagnostic run (2026-05-30)

Ran the live demo on a throwaway Xvfb (`:99`) with the four toggles above
plus an `import -window root` capture of the framebuffer at frame ~120. The
result:

| Source | What it reports | Pixel content |
|--------|-----------------|---------------|
| Atom's exported `VkImage` (`O3deBackend.cc`) | handle `0x777294873ed0`, 512x512 | -- |
| Imported VkImage on Qt's device (`vkimport DEDICATED import OK`) | handle `0x63ccc62a0690`, 512x512 | -- |
| `RenderTextureMetalId` handoff (`O3deCamera.cc`) | handle `0x63ccc62a0690`, 512x512 | -- |
| Qt's `fromNative` wrap (`[gz-gui-diag] CreateTexture`) | handle `0x63ccc62a0690` (every frame), size `0x0` -> `1024x1024` -> `1024x670` (window-sized) | -- |
| `GZ_O3DE_DUMP_PNG` (transfer-copy readback, Qt's device) | 512x512 PPM | **67 unique colours**, centre pixel `(102, 102, 102)`, shapes present |
| `GZ_O3DE_SAMPLE_PROBE` (sampler readback, Qt's device) | 512x512 PPM | **67 unique colours**, byte-identical to transfer dump |
| Xvfb root capture (the actual presented framebuffer) | 1280x800 PNG | **uniform `(148, 148, 148)`** -- Atom's clear colour |

So the imported VkImage on Qt's device carries the producer's correct
shapes (proven two independent ways: memory transfer copy AND sampler-path
compute readback). Qt's `fromNative` wraps the correct VkImage handle every
frame. Yet the swapchain-presented framebuffer is uniform Atom-clear.

Crucially: the centre-pixel value in the dumps `(102, 102, 102)` does not
match the screen value `(148, 148, 148)`. So Qt is rendering content
*different from* what the imported VkImage actually contains -- not just a
washed-out version of the same content.

### What does NOT reproduce the symptom in isolation

The gz-gui regression test added four cases (all four pass against
`QQuickWindow::grabWindow`):

* Test 4a -- size mismatch (`fromNative` told 2x larger than VkImage extent): harmless.
* Test 4 (the current 4b) -- recreate-every-frame `fromNative` wrapper around the same VkImage: harmless.

Combined with Tests 1-3 (LINEAR / OPTIMAL single-device / cross-device FD
import all PASS), no isolated test reproduces the bug.

**Important caveat**: an Xvfb capture of Test 3's passing run shows a pure
64x64 white block on screen -- not the four-quadrant pattern that
`grabWindow()` reports as correct. So `grabWindow()` and the swapchain
presentation diverge: the regression tests' assertion on `grabWindow()`
is not the same as what the user sees. The production bug lives on the
swapchain-present path that `grabWindow()` does not exercise.

### Next investigation -- on-screen verification

The next test variant must capture the actual swapchain framebuffer (Xvfb
root grab or RenderDoc swapchain capture), not the grabWindow output, and
assert the pattern there. If a single-device test reproduces the uniform-
output symptom under on-screen capture, we have a self-contained bug repro.
If even the on-screen capture shows the pattern correctly, the bug requires
the full production threading model (Atom thread + gz-rendering render
thread + Qt scene-graph thread + MinimalScene's NewTexture handoff).
