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
| 25 | Stage B: render live scene directly into the exportable image (Phase 2) | 🔶 in progress, **blocked** (not by #26) | Atom renders the scene into the export image every frame via `CreateRenderPipelineForImage`. The render-into works (fixed the single-sample `MainPipeline` MSAA crash → 4× MSAA). Now blocked on a **cross-device render-target/compression handoff**: Qt's separate `VkDevice` faults the first time it samples the producer's compressed colour-attachment image (the plain static-probe write samples fine). See the findings doc, hypothesis 12. |
| 26 | Export render-finished timeline semaphore (RHI::Fence) + consumer wait | ✅ **done — implemented & proven**, but NOT the live-path fix | Producer signals a timeline `RHI::Fence` each frame (`RHISystemNotificationBus::OnFramePrepare` + `ImportScopeProducer` + `FrameGraphInterface::SignalFence`; `usedForWaitingOnDevice=true` makes it a `TimelineSemaphoreFence`); consumer imports it as a timeline semaphore and waits on the per-frame value. Verified: the shared timeline counter advances across both `VkDevice`s and waits are satisfiable. The earlier belief that this missing semaphore *was* the device-loss cause is **disproven** — the loss persists with #26 working (see hypothesis 8). Gated by `GZ_O3DE_INTEROP_SEM`. |

**Dependency direction (corrected):** #26 (sync) is done and proven, but it did **not**
unblock #25. The earlier "host-sync is spec-insufficient → #25 needs #26" framing was
wrong: the live device loss is a render-target/compression handoff problem, orthogonal
to synchronization. #25 now depends on a producer-side decompress / plain GPU-copy
handoff, not on the semaphore. See the findings doc's "Candidate fixes" section.

## See also

- [zero-copy-interop-findings.md](zero-copy-interop-findings.md) — root-cause analysis
  of the Phase 2 device loss and the confirmed-feasible #26 design.
- [../M4_INTEROP_DESIGN.md](../M4_INTEROP_DESIGN.md) — the M4 interop design (original
  Vulkan→GL plan + the native Vulkan→Vulkan update).
- [../IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md) — overall PoC plan + M0–M4
  milestones.
