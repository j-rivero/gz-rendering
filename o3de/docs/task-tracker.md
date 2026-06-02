# Task numbering provenance (#20–#26)

Commit messages and [zero-copy-interop-findings.md](zero-copy-interop-findings.md)
refer to tasks by number — `#25`, `#26`, etc. This file records what those numbers
are and what each one means, so the references are decipherable later.

## Where the numbers come from

They are IDs from the **interactive coding session's task tracker** (the assistant's
`TaskCreate`/`TaskList` tooling) used to plan and track the **native Vulkan→Vulkan
zero-copy interop** sub-effort. They are session-scoped planning IDs, not GitHub
issues and not the milestone numbers. Numbering happens to start at 20 because earlier
session tasks (the M0–M3.5 PoC bring-up) were completed and cleared before this
sub-effort began; only #20–#26 remain.

**Do not confuse with the `M0`–`M4` milestones** in
[../IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md): those track the overall PoC
(M0 plumbing → M3.5 live gz-gui display → M4 zero-copy). The #20–#26 tasks are the
fine-grained breakdown of **M4's** native-Vulkan display path.

## The tasks

| # | Subject | Status | Meaning |
|---|---------|--------|---------|
| 20 | gz-gui: enable external-memory/semaphore exts on Qt's Vulkan device | ✅ done | Patched gz-gui (`j-rivero/gz-gui` branch `o3de_vulkan_interop`) runs Qt on the Vulkan RHI and enables `VK_KHR_external_memory_fd` / `VK_KHR_external_semaphore_fd` on Qt's `VkDevice`, and injects its device handles into `MinimalScene`. |
| 21 | gz-rendering: reusable "import into provided VkDevice" helper | ✅ done | `O3deVkImport.{hh,cc}` — import an exported `OPAQUE_FD` image (and the ownership-acquire barrier) onto a caller-provided `VkDevice`. |
| 22 | gz-rendering: O3deRenderEngine reads injected Qt Vulkan device | ✅ done | The engine picks up Qt's injected `VkInstance`/`VkPhysicalDevice`/`VkDevice`/queue so the consumer-side import targets Qt's device. |
| 23 | gz-rendering: O3deCamera RenderTextureMetalId + PrepareForExternalSampling | ✅ done | Consumer hooks: hand the imported `VkImage` to gz-gui (`RenderTextureMetalId`) and transition it for sampling each frame (`PrepareForExternalSampling`). |
| 24 | Verify static probe image displays in gz-gui via native V→V (Phase 1) | ✅ done | Atom uploads a gradient into the export image once; Qt imports + samples it. Verified displaying via native Vulkan→Vulkan. |
| 25 | Stage B: render live scene directly into the exportable image (Phase 2) | ✅ **done — WORKING** | Atom renders the scene into the export image every frame via `CreateRenderPipelineForImage`; Qt samples it zero-copy. Verified ~2000 frames at ~60-100 fps across multiple window resizes (gen 1→2→3), zero device loss. Took three fixes: the 4× MSAA crash fix; the **consumer retiring old imports** (the real device-loss fix — freeing an import under Qt's in-flight frame was the bug); and #26 for per-frame cross-device sync. The render-target/compression theory was disproven by `GZ_O3DE_NO_RESIZE` (a fixed-size live image samples fine for 2000 frames). See the findings doc, hypotheses 12-15. |
| 26 | Export render-finished timeline semaphore (RHI::Fence) + consumer wait | ✅ **done — implemented & proven** | Producer signals a timeline `RHI::Fence` each frame (`RHISystemNotificationBus::OnFramePrepare` + `ImportScopeProducer` + `FrameGraphInterface::SignalFence`; `usedForWaitingOnDevice=true` makes it a `TimelineSemaphoreFence`); consumer imports it as a timeline semaphore and waits on the per-frame value. Verified: the shared timeline counter advances across both `VkDevice`s in lockstep every frame. Required infrastructure for the working live path — though NOT the device-loss trigger (that was #25's resize/re-import; see hypothesis 8). Gated by `GZ_O3DE_INTEROP_SEM`. |

**Dependency direction (final):** #26 (sync) and #25 (live render + the resize/re-import
fix) are both done and the live zero-copy path works. The earlier framings — "#25 needs
#26 for sync", then "#25 blocked on a compression handoff" — were both wrong; the actual
device-loss cause was a consumer-side use-after-free on resize (freeing an imported
VkImage under Qt's in-flight frame), fixed by retiring old imports. See the findings
doc's "Actual root cause" + "The fix" sections.

## Post-M4 investigations

| # | Task | Status | Notes |
|---|------|--------|-------|
| 27 | Intermittent grey viewport — characterise + mitigate | ✅ done (workaround) | Per-launch, COLD-START WSI present race; input-independent (greys 1- and 2-light, with/without shadows/directional alike). Mitigated by `GZ_SWAPCHAIN_FORCE_IDLE` (bounded `vkDeviceWaitIdle` primer, gz-gui `fa385042` + `0a5d66f0`). Measure via CENTRE viewport crop, not full frame. |
| 28 | Proper gz-gui RHI fix for the grey | ❌ not possible in gz-gui | Ruled out both candidates: (1) a present-time fence — Qt's `endFrame` submits+presents atomically and exposes NO signal between them; (2) make the composite target the swapchain — fails at any DPR/size (composite always → 1200×902 intermediate, swapchain always `draws=0`). Bug is in Qt 6.4.2 QRhi-Vulkan present. Fix = upgrade Qt ≥6.8 (deferred 2026-06-02) or keep the layer primer. See findings doc 2026-06-02. |

## See also

- [zero-copy-interop-findings.md](zero-copy-interop-findings.md) — root-cause analysis
  of the Phase 2 device loss and the confirmed-feasible #26 design.
- [../M4_INTEROP_DESIGN.md](../M4_INTEROP_DESIGN.md) — the M4 interop design (original
  Vulkan→GL plan + the native Vulkan→Vulkan update).
- [../IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md) — overall PoC plan + M0–M4
  milestones.
