# M4 — zero-copy Vulkan↔GL interop (design & feasibility)

**Status: design / feasibility complete; implementation not started.** This is
the post-PoC performance milestone from [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).
It records the validated design so the implementation can proceed (or be picked
up) with the unknowns already resolved.

## Goal

Replace the CPU-readback display path with a **zero-copy** one. Today the live
display goes gz-gui → `camera->Copy(image)` → `O3deRenderTarget::Copy` →
`O3deBackend::RenderFrame` → Atom `FrameCapture` GPU→CPU readback → `memcpy` →
gz-gui `glTexSubImage2D` (GPU→CPU→GPU). gz-gui itself warns this path is slow.
M4 shares Atom's Vulkan-rendered colour image with Qt's GL context as a GL
texture, so no pixels ever travel through the CPU.

## How gz-gui chooses the path (the hook we target)

`gz-gui/.../minimal_scene/EngineToQtInterface.cc`:
`NeedsFallback()` is true when `engine->GraphicsAPI()` is **not** `OPENGL`/`METAL`.
On the non-fallback path gz-gui calls **`camera->RenderTextureGLId()`** and binds
that GL texture in Qt's GL context. So M4 = (a) report `GraphicsAPI() == OPENGL`
and (b) implement `O3deCamera::RenderTextureGLId()` to return a GL texture that
*aliases* Atom's Vulkan render target. (Today the engine reports `VULKAN` on
purpose, to force the readback fallback; see `O3deRenderEngine::GraphicsAPI`.)

## Feasibility — all blockers resolved

| Requirement | Status | Evidence |
|---|---|---|
| GL external-memory/semaphore import | ✅ present | `GL_EXT_memory_object_fd`, `GL_EXT_semaphore_fd`, `GL_EXT_import_sync_object` (glxinfo, NVIDIA) |
| Vulkan external-memory/semaphore export | ✅ present | `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd` (vulkaninfo) |
| Atom native handle access | ✅ exposed | `RHI.Interface/Vulkan/RHIVulkanInterface.h`: `GetDeviceNativeHandle`, `GetPhysicalDeviceNativeHandle`, `GetNativeImage`, `GetImageMemory`, `GetImageAllocation{Size,Offset}`, `GetFenceNativeHandle` (semaphore) |
| Exportable images **without patching O3DE** | ✅ via EBus | `Device::BuildImageCreateInfo` adds `VkExternalMemoryImageCreateInfo` when `ExternalHandleRequirementBus::CollectExternalMemoryRequirements` returns a flag (`Device.cpp:~1945`) |
| Exportable **backing memory** (the subtle one) | ✅ via same EBus | VMA allocator is created with `pTypeExternalMemoryHandleTypes` populated from the *same* bus (`Device.cpp:1698-1707`). So all VMA memory becomes exportable. |
| Exportable timeline semaphore for sync | ✅ via same EBus | `TimelineSemaphoreFence.cpp:59-65` adds `VkExportSemaphoreCreateInfoKHR` when `CollectSemaphoreExportHandleTypes` returns a flag; Linux `GetSemaphoreFdKHR` exists |

**Key result:** connecting **one** `AZ::Vulkan::ExternalHandleRequirementBus::Handler`
that returns `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` (memory) and
`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT` (semaphore) — **before the RHI
device is created** — makes every Atom image and timeline semaphore exportable.
No fork of O3DE is required. (No gem in this checkout connects the bus, so it is
dormant and ours; it was evidently designed for the OpenXRVk swapchain-sharing
path.)

> Timing constraint: the VMA allocator captures the external types **at device
> creation**, so the handler must be connected before `GameApplication::Start()`
> (between `new GameApplication(...)` and `Start(...)` in `BootstrapOnThread`).

## Architecture

```
Render thread (ours, Vulkan)                 gz-gui render thread (Qt GL context)
────────────────────────────                 ────────────────────────────────────
ExternalHandleRequirementBus::Handler
  → all images + semaphores exportable
Atom renders the RTT "Output" image
  (a normal Atom VkImage, now exportable)
GetNativeImage / GetImageMemory
vkGetMemoryFdKHR(memory)        ───── dup'd FD ─────►  glImportMemoryFdEXT
                                                       glTextureStorageMem2DEXT(tex, …)
per frame:                                             (one-time, on the GL thread)
  Atom signals its frame timeline
  semaphore after the RTT pass
  vkGetSemaphoreFdKHR           ───── FD ───────────►  glImportSemaphoreFdEXT
                                                       per frame:
                                                         glWaitSemaphoreEXT(tex layout)
                                                         sample `tex` → RenderTextureGLId()
                                                         glSignalSemaphoreEXT (release)
```

* **Image FD is exported once** (stable for the image's lifetime) and imported
  into a single GL texture object; only re-done on resize (new RTT image).
* **Synchronisation is per frame** via a shared timeline semaphore: Vulkan
  signals after the render, GL waits before sampling, then signals back so
  Vulkan can reuse the image. Skipping this races the GPU and tears/corrupts.
* **Layout:** import the image to GL with the matching tiling
  (`GL_OPTIMAL_TILING_EXT` vs `LINEAR`) and use `glWaitSemaphoreEXT`'s
  `srcLayouts` to hand off the Vulkan image layout. Getting tiling/layout wrong
  yields garbage; this is the most error-prone part.

## gz-rendering integration points

| Piece | Change |
|---|---|
| `O3deRenderEngine::GraphicsAPI()` | return `OPENGL` (interop) instead of `VULKAN` (readback). Gate behind a runtime flag so the readback path stays available as a fallback. |
| `O3deCamera::RenderTextureGLId()` | override to return the imported GL texture id (currently `BaseCamera` default logs an error). |
| `O3deBackend` | new entry points: `ExportRenderTargetFd()` (render thread) and `ImportToGlTexture()` (GL thread); the bus handler; the shared-semaphore plumbing. The plain-C++ `O3deBackend` interface must not leak Vulkan/GL types — pass the FD as an `int` and the GL id as a `unsigned int`. |
| GL-context thread | the GL import/sample calls **must** run on gz-gui's render thread with the Qt GL context current — *not* our Vulkan render thread. `RenderTextureGLId()` is already called there. |

## Recommended implementation order

1. **Foundation (verifiable headlessly):** connect the `ExternalHandleRequirementBus`
   handler before `Start()`; after a frame, fetch the RTT "Output" `DeviceImage`,
   call `GetImageMemory` + `vkGetMemoryFdKHR`, and assert a valid FD is returned.
   This proves the exportable-image foundation **without any GL or display** (FD
   ≥ 0 is the pass/fail signal) — the right first PR.
2. **GL import:** on the GL thread, import the FD to a GL texture
   (`glImportMemoryFdEXT` + `glTextureStorageMem2DEXT`); sample it into a tiny
   offscreen FBO and read back a few pixels to compare against the existing
   readback path (still no gz-gui needed).
3. **Semaphore sync:** export Atom's frame semaphore, import to GL, add the
   wait/signal around the sample.
4. **Wire gz-gui:** flip `GraphicsAPI()`→`OPENGL`, return the texture from
   `RenderTextureGLId()`, verify the live viewer.

## Implementation progress

**Step 1a — DONE & verified (foundation).** A header-only
`AZ::Vulkan::ExternalHandleRequirementBus::Handler` (`GzExternalHandleProvider`
in `O3deBackend.cc`) is connected before `GameApplication::Start()`, gated by
**`GZ_O3DE_INTEROP`** (off by default → the readback path is untouched). Verified:
with it on, the runtime still boots and renders the box/sphere/cylinder (960×720
→ readback), exit clean, no Vulkan/VMA errors, and the log confirms Atom queried
the bus — `Atom queried external-memory requirements -> requesting OPAQUE_FD
(images become exportable)`. So the RTT image and its VMA-backed memory are now
created exportable. (The *semaphore* collector was not hit in the short offscreen
run — the offscreen path appears to use binary fences, not timeline semaphores;
revisit when wiring sync in step 3, possibly creating our own exportable
semaphore.)

**Step 1b — BLOCKED on a linkage decision (the real M4 obstacle).** Proving the
FD export (`vkGetMemoryFdKHR` on the RTT image) needs the native handles
(`AZ::Vulkan::GetNativeImage` / `GetImageMemory` / `GetDeviceNativeHandle`).
Those live in `RHIVulkanInterface.cpp`, compiled into the **static** library
`Gem::Atom_RHI_Vulkan.Interface`, which PUBLIC-depends on the whole
`.Private.Static`. Our plugin loads the Vulkan RHI as a **runtime gem `.so`**, so
linking that static lib pulls a *second* copy of the Vulkan RHI into the plugin
(duplicate AZ type registration / static state) — a real risk to the working
PoC. The functions themselves are thin casts
(`static_cast<Vulkan::Image&>(img).GetNativeImage()`), so the candidate
approaches are:
  1. **Reimplement the 2–3 accessors inline** in `O3deBackend.cc` by including the
     Vulkan RHI *Source* internal headers (`RHI/Image.h`, `RHI/Device.h`,
     `MemoryView.h`) and doing the cast ourselves — *if* `GetNativeImage()` /
     `GetMemoryView().GetNativeDeviceMemory()` are inline (no extra link). Lowest
     duplication risk; needs the internal include dirs (+ glad/vma) to compile.
  2. **Link `Gem::Atom_RHI_Vulkan.Interface` whole-archive** and rely on AZ's
     UUID-based `azrtti_cast` + identical layout (same source/flags) making the
     cross-module cast valid. Simplest to wire; carries the duplicate-static-state
     risk — must validate no double-registration asserts.
  3. **Add a tiny exporter to the GzAtomPoc gem** (built inside the O3DE tree,
     where linking `.Interface` is natural) that exposes the FD via a clean C ABI
     / EBus the plugin calls. Cleanest separation; most plumbing.

Recommendation: try (1) first (least risk to the shipping PoC); fall back to (3).

## Risks / open questions

* **Layout & tiling hand-off** (step 2/3) is the classic interop failure mode;
  budget iteration here. Start with `LINEAR` tiling for the shared image if
  optimal-tiling import misbehaves.
* **Resize:** re-export + re-import on every RTT resize; tear down the old GL
  texture/semaphore on the GL thread.
* **Thread/context discipline:** Vulkan work stays on our render thread, all GL
  work on gz-gui's GL thread. Crossing them corrupts state.
* **Verification here is limited:** the display itself can only be confirmed in
  gz-gui; steps 1–2 are designed to be verifiable by FD validity + pixel
  read-back so most of M4 can be proven without a screenshot.
* Keep the CPU-readback path intact behind a flag — M4 is an optimisation, not a
  replacement, until proven robust across drivers.
