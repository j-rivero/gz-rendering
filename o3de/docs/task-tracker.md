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
| 25 | Stage B: render live scene directly into the exportable image (Phase 2) | 🔶 in progress, **blocked by #26** | Atom renders the scene into the export image every frame via `CreateRenderPipelineForImage`. The render-into works (fixed the single-sample `MainPipeline` MSAA crash → 4× MSAA); blocked on #26 for cross-device stability. |
| 26 | Export render-finished timeline semaphore (RHI::Fence) + consumer wait | ⏳ pending (next) | The producer→consumer GPU semaphore needed so Qt safely samples the live shared image. Confirmed **required** (host-sync is spec-insufficient → GPU TDR) and confirmed **feasible** via public RHI API. See the findings doc. |

**Dependency direction:** #25 depends on #26 (the live path needs the semaphore to be
stable). The tracker may also show a stale reverse link (#26 blocked-by #25) left over
from the original "render-into first, then add sync" framing — ignore it; #26 is the
next actionable task.

## See also

- [zero-copy-interop-findings.md](zero-copy-interop-findings.md) — root-cause analysis
  of the Phase 2 device loss and the confirmed-feasible #26 design.
- [../M4_INTEROP_DESIGN.md](../M4_INTEROP_DESIGN.md) — the M4 interop design (original
  Vulkan→GL plan + the native Vulkan→Vulkan update).
- [../IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md) — overall PoC plan + M0–M4
  milestones.
