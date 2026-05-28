# O3DE↔Qt native Vulkan zero-copy interop — root-cause findings & #26 design

Status: **Phase 2 (live zero-copy) blocked on cross-device GPU synchronization.**
Date: 2026-05-28. Author: bring-up debugging session.

This documents (a) the confirmed root cause of the live-path device loss, (b) every
hypothesis tested and the evidence that eliminated it, and (c) the confirmed-feasible
design for the render-finished semaphore (#26) — the one remaining fix needed to make
the live zero-copy path stable. It exists so the zero-copy build can resume in a
deliberate later session without re-deriving any of this.

## Goal

Display O3DE/Atom's live offscreen render in gz-gui's MinimalScene via **native
Vulkan→Vulkan zero-copy**: Atom (its own `VkDevice`, "device A") renders into an
exportable colour image; gz-gui/Qt (a separate `VkDevice`, "device B") imports that
image (`OPAQUE_FD`) and samples it with no CPU readback. This is milestone **M4**
(post-PoC, optional) in the plan; the CPU-readback path is the plan's primary display
path and already works.

Two phases:
- **Phase 1 — static probe (DONE, stable):** Atom uploads a gradient into the export
  image *once*; Qt imports + samples it. Verified displaying via native V→V.
- **Phase 2 — live (#25 + #26):** Atom renders the scene into the export image *every
  frame* (`CreateRenderPipelineForImage`). This is where the device loss occurs.

## Symptom

With `GZ_O3DE_INTEROP=1 GZ_O3DE_INTEROP_LIVE=1` + Vulkan GUI backend:
the live scene renders correctly into the shared image, the consumer imports it, then
~5 seconds later:

```
[gz-o3de] interop live frame 1/2/3: 800x502 gen=2 ... ms waitIdle=0
   (~5.0 s of total silence)
[QT] Device loss detected in vkQueueSubmit()
[QT] Graphics device lost, cleaning up scenegraph and releasing RHI
   ... later, on Qt's device-loss recovery:
QSGVulkanTexture::fromNative(...)  →  SIGSEGV in libnvidia-glcore  (exit 139)
```

The final `fromNative` segfault is a *consequence* (Qt re-imports the now-stale VkImage
on a freshly recreated device after the loss). The primary event is the device loss.

## Confirmed root cause

**Cross-device external-memory synchronization is missing.** Qt (device B) samples an
image whose memory is written by Atom (device A). Vulkan's external-memory model
requires the producer to **signal a semaphore** after writing that the consumer
**waits on** before reading, to make the writes available/visible across the device
boundary. We provide none (`semaphoreFd = -1`) and rely on `vkDeviceWaitIdle` on the
*producer*. Host-sync only orders device A's own work — per spec it does **not**
establish cross-device visibility. Qt's GPU job that samples the shared image therefore
waits on a dependency that is never satisfied → it hangs → the NVIDIA kernel watchdog
resets the GPU at ~5 s (TDR-style) → every logical device on the shared physical GPU
reports `VK_ERROR_DEVICE_LOST`.

This is exactly the render-finished semaphore deferred as **#26**.

### Why the static probe works but the live path does not
The static image is written **once** (a transfer/upload) long before Qt imports it, and
never touched again — a single, settled cross-device handoff. The live image is a
*pipeline output* re-rendered each frame; the repeated producer-write → consumer-read
transitions without a real GPU dependency are unsound, and even after rendering stops
the image's last write is never made cross-device-available, so Qt's sampling hangs.

## Hypotheses tested and eliminated (systematic debugging)

| # | Hypothesis | Test | Result |
|---|-----------|------|--------|
| 1 | `MainPipeline` forced single-sample → deferred G-buffer shaders need MSAA | Set 4× MSAA on the live `CreateRenderPipelineForImage` descriptor + `SetApplicationMultisampleState` (mirror the CPU-readback path) | ✅ **Real bug, FIXED.** Flood of `DeviceShaderResourceGroupData` "multisample count 1 but shader expected more than 1" SRG errors → **0**. Pipeline now renders clean at 4×. Device loss persisted → separate issue. |
| 2 | `GzAtomPoc` probe (a 2nd scene + `MainPipelineRenderToTexture` pipeline + `CapturePassAttachmentWithCallback`, on the shared device) | Env-gate the probe off when `GZ_O3DE_INTEROP` is set (`GzAtomPocSystemComponent::Activate`) | ✅ Ruled out — loss unchanged. |
| 3 | Unmatched `VK_QUEUE_FAMILY_EXTERNAL` ownership *acquire* in `PrepareForExternalSampling` (no matching producer release; spec-invalid) | `GZ_O3DE_NO_QFOT=1` → plain `VK_QUEUE_FAMILY_IGNORED` layout barrier | ✅ Ruled out — loss unchanged. |
| 4 | Producer-side GPU fault | Log `vkDeviceWaitIdle` return per frame | ✅ Ruled out — `waitIdle=VK_SUCCESS` every frame; producer healthy. |
| 5 | The ~5 s == our consumer `RenderFrameForInterop` `wait_for(5s)` timeout | Log on timeout; log every frame START | ✅ Ruled out — no `TIMED OUT`, no "frame 4 START"; the consumer simply goes idle after 3 frames (static scene). The 5 s is external (GPU). |
| 6 | Atom keeps submitting GPU work on idle ticks (`app->Tick()` every 16 ms) and races Qt's sampling of the shared image | `GZ_O3DE_NO_IDLE_TICK=1` → skip idle ticks on the live path | ✅ Ruled out — loss unchanged even with Atom fully idle after frame 3. |
| 7 | A single-device Vulkan correctness/sync bug | Force `VK_LAYER_KHRONOS_validation` + `SYNCHRONIZATION_VALIDATION` on all instances | ✅ Ruled out as the cause — **zero** findings name our image; all SYNC-HAZARDs are Atom-internal transient attachments (`MSAAResolve*`, `SkyBox`, `DisplayMapper`, `Shadows…`), well-known false positives. Validation layers track each `VkDevice` independently and **do not model cross-device external-memory hazards** — so the real hazard is invisible to them (this also explains the earlier "0 errors"). |

Net: every cheap/local cause is eliminated; one real bug (MSAA) was fixed; the remaining
device loss is the cross-device sync (#26).

## Confirmed-feasible #26 design (no Pass subclassing)

The earlier blocker — that the `SignalFence` / `ImageAttachmentCopy` **pass** scopes are
`final`/`protected` — was the *wrong* route. The render-finished fence is implementable
with **public RHI API**, exactly as `AttachmentReadback` and the `AFR` gem already do it.

Producer (Atom-owning TU, `O3deBackend.cc`):
1. Create once: `RHI::Ptr<RHI::Fence> m_frameFence; m_frameFence->Init(deviceMask, RHI::FenceState::Reset, /*usedForWaitingOnDevice*/…)`. For cross-`VkDevice` export use the `FenceFlags::CrossDevice` path (needs `DeviceFeatures::m_crossDeviceFences`); otherwise export the timeline `VkSemaphore` FD directly.
   - `Gems/Atom/RHI/Code/Include/Atom/RHI/Fence.h` (`Init` ~L35, `Reset` ~L48).
2. Build a standalone `RHI::ScopeProducerFunctionNoData` (Prepare/Compile/Execute). In **Prepare**, call `frameGraph.SignalFence(*m_frameFence)`.
   - `FrameGraphInterface::SignalFence` — `…/RHI/FrameGraphInterface.h:262`.
   - Pattern: `AttachmentReadback.cpp` (creates fence L43-45, standalone scope L86-90, `SignalFence` L240, `ImportScopeProducer` L472, `WaitOnCpuAsync` L384, `Reset` L423). `AttachmentReadback` is **not** a Pass subclass.
3. Connect `RHI::RHISystemNotificationBus::Handler`; in `OnFramePrepare(FrameGraphBuilder& b)` call `b.ImportScopeProducer(myScope)`.
   - Bus declared `…/RHI/RHISystemInterface.h:91-106`; broadcast every frame from `RHISystem::FrameUpdate` (`…/RHI/RHISystem.cpp:264-294`) **after** RPI pass registration, so the scope orders at frame-end.
   - **Ordering caveat:** a lone fence-signal scope with no attachment use may be scheduled early/in parallel. To guarantee it runs after the interop image's last write, have the scope `UseAttachment(...)` on the pipeline's output attachment, or `ExecuteAfter(finalScopeId)`. (`AttachmentReadback` orders via its copy attachment.)
4. Per frame after Execute: read `AZ::Vulkan::GetFenceNativeHandle(*m_frameFence->GetDeviceFence(idx))` (stable timeline `VkSemaphore`, exported once via `vkGetSemaphoreFdKHR` OPAQUE_FD) + `AZ::Vulkan::GetFencePendingValue(...)` (the value the GPU will signal this frame), publish that value, then `m_frameFence->Reset()` (advances pending value for next frame).
   - Accessors: `…/RHI/RHI.Interface/Vulkan/RHIVulkanInterface.h:45-46` (already patched to default visibility).
   - Lifecycle: `TimelineSemaphoreFence` inits `m_pendingValue=1`; `ResetInternal()` does `m_pendingValue++`.

Consumer (`O3deCamera` / `O3deVkImport.cc`):
5. Import the timeline `VkSemaphore` once (`VkImportSemaphoreFdInfoKHR`, OPAQUE_FD) into `O3deVkImportedImage::semaphore`.
6. `O3deVkAcquireFromProducer` already adds `_img.semaphore` as a wait semaphore — **but for a timeline semaphore it must add `VkTimelineSemaphoreSubmitInfo` with the per-frame wait value** (`semaphoreWaitValue`, already plumbed in `O3deInteropImport`). The host-waited fence after that submit then guarantees the frame is available before Qt samples.

Prereqs already done: `#20` enabled external-memory + external-semaphore extensions on
Qt's `VkDevice`. **Verify** Qt's device also enables `timelineSemaphore` (VK 1.2 core or
`VK_KHR_timeline_semaphore`); if not, either enable it or fall back to a binary external
semaphore (stricter single signal/wait per frame).

## Diagnostic toggles left in the tree (for resuming #26)

- `GZ_O3DE_NO_QFOT=1` — consumer acquire uses a plain layout barrier instead of the
  EXTERNAL queue-family ownership transfer (`O3deVkImport.cc`).
- `GZ_O3DE_NO_IDLE_TICK=1` — render thread does not tick Atom while idle on the live
  path (`O3deBackend.cc::RenderThreadMain`).
- Per-frame `interop live frame N … waitIdle=<VkResult>` logging + a
  `RenderFrameForInterop TIMED OUT` log (`O3deBackend.cc`).
- Validation run recipe: set `VK_LAYER_PATH` + `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
  + `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
  (layer at `vendor/o3de/build/linux/bin/profile/`).

## Fallback if #26 is deferred again

Drive the **live** scene through the proven `O3deRenderTarget::Copy()` /
`AttachmentReadback` path (CPU readback → Qt `glTexSubImage2D` upload). Stable, slower,
already implemented for the static/Copy path; this is the plan's primary display path.
Zero-copy then remains a clean, isolated follow-up gated only on #26.
