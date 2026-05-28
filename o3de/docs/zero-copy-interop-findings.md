# O3DE↔Qt native Vulkan zero-copy interop — root-cause findings & #26 outcome

Status: **#26 (render-finished timeline semaphore) implemented & proven working.
Phase 2 (live zero-copy) still blocked — but on a *different* cause than first
thought: a cross-device render-target/compression handoff, not synchronization.**
Date: 2026-05-28. Author: bring-up debugging session.

This documents (a) the #26 implementation and the evidence it works, (b) the
**correction** to the earlier root-cause conclusion (the missing semaphore was
necessary infrastructure but NOT the device-loss cause), (c) every hypothesis tested
and the evidence that eliminated it, and (d) the real remaining blocker. It exists so
the zero-copy build can resume deliberately without re-deriving any of this.

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
- **Phase 2 — live (#25 + #26):** Atom renders the scene into the export image *every
  frame* (`CreateRenderPipelineForImage`). This is where the device loss occurs.

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

## CORRECTED root cause (the earlier conclusion was wrong)

The earlier version of this doc concluded the device loss *was* the missing
render-finished semaphore. **That was disproven**: with #26 implemented and the
semaphore proven working, the live path still loses the device, in the same place.

The real behaviour, established this session:
- The **consumer's** acquire `vkQueueSubmit` returns `VK_ERROR_DEVICE_LOST` in **0 ms**
  — it does **not** hang. The device was already dead before the submit (a hung wait
  would have taken ~5 s). So it is **not** a sync/TDR on our side.
- The **producer** stays healthy throughout (`waitIdle=VK_SUCCESS` every frame). Only
  **Qt's** logical device dies — not a whole-physical-GPU TDR.
- The device dies the first time **Qt's own render pass samples the producer's
  pipeline-rendered image**. The **static probe** — a *plain* `UpdateImageContents`
  write — is sampled forever without issue; the **live** path renders the image as a
  **colour attachment** (a render target, with NVIDIA framebuffer/DCC compression
  state). Qt's separate `VkDevice` samples that compressed render-target image without
  the producer's compression context → Qt's device faults.

**Conclusion:** the remaining blocker is a **cross-device render-target / compression
handoff** problem, orthogonal to synchronization. The render-finished semaphore is
necessary infrastructure for a correct zero-copy pipeline, but it is not what was
losing the device.

### Why the static probe works but the live path does not
The static image is written **once** by a plain transfer (`UpdateImageContents`),
leaving it in an ordinary `SHADER_READ_ONLY_OPTIMAL`, uncompressed, cross-device-
samplable state. The live image is a **pipeline colour-attachment output**; its
on-device representation (compression metadata / render-target state) is not valid for
a *different* `VkDevice` to sample, regardless of layout barriers or semaphores.

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
| 11 | Consumer submit hangs (sync/TDR) vs. fails immediately (invalid op) | Time the submit + fence wait | ✅ **Decisive:** `submit=0 ms`, `VkResult=-4`. Immediate failure → device already dead → not our wait. |
| 12 | What survives: cross-device sampling of a **compressed colour-attachment render target** (live) vs a **plain** write (static probe) | Static probe (plain `UpdateImageContents`) samples forever; live (pipeline colour attachment) dies on first Qt sample | ⛳ **Remaining root cause** (not yet fixed — see below). |

Net: synchronization (#26) is implemented and works; the device loss is the
render-target/compression handoff (hypothesis 12).

## Candidate fixes for the remaining blocker (not yet attempted)

1. **One-copy plain handoff (most likely to work):** render the scene into Atom's
   normal pipeline target (same-device, compressed — fine), then GPU-copy it into the
   shared exportable image as a *plain* image each frame — the exact state the static
   probe proves Qt can sample. Still GPU-only (no CPU readback); one extra GPU copy.
   Keeps the #26 semaphore for the producer→consumer "frame ready" edge.
2. **Decompress in place (true zero-copy):** keep rendering directly into the shared
   image but force a producer-side decompress/resolve to a cross-device-samplable
   state (transition to `GENERAL`, or disable DCC on the image). Preserves zero-copy
   but uncertain: depends on whether the driver makes the decompressed result visible
   to a separate `VkDevice`.

A correct zero-copy design likely also needs the **reverse** edge (consumer→producer
"done sampling") or double-buffering, so the producer never overwrites a frame Qt is
still reading — out of scope for the single shared image used today.

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
- `GZ_O3DE_NO_QFOT=1` — consumer acquire uses a plain layout barrier instead of the
  EXTERNAL queue-family ownership transfer (`O3deVkImport.cc`).
- `GZ_O3DE_NO_IDLE_TICK=1` — render thread does not tick Atom while idle on the live
  path (`O3deBackend.cc::RenderThreadMain`).
- Per-frame `interop live frame N … fence=… value=…` (producer) and
  `vkimport: acquire wait=… shared-counter=… submit=…` (consumer) logging.
- Validation: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` +
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
  (layer at `vendor/o3de/build/linux/bin/profile/`).

## Fallback (the plan's primary path)

Drive the **live** scene through the proven `O3deRenderTarget::Copy()` /
`AttachmentReadback` path (CPU readback → Qt `glTexSubImage2D`). Stable, slower, already
implemented. Native zero-copy then remains a clean follow-up gated on the
render-target-handoff fix above (not on #26, which is done).
