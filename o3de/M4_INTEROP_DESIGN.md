# M4 — zero-copy Vulkan↔GL interop (design & feasibility)

**Status: steps 1–2 (export + GL import) DONE & verified; steps 3–4 not started.**
This is the post-PoC performance milestone from
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). It records the validated design
plus the implemented/verified pieces: with `GZ_O3DE_INTEROP=ON` (build, **now the
default**) and `GZ_O3DE_INTEROP=1` (runtime) an Atom-created image exports a valid
OS FD via `vkGetMemoryFdKHR` (step 1), **and** that FD imports into a GL texture
that reads back bit-exact (step 2, `GZ_O3DE_INTEROP_GLTEST=1`: all 65536 texels
of a known gradient matched). Remaining work: render the live scene into the
shared image, frame-semaphore sync, and gz-gui wiring (steps 3–4). Because interop
is the default build, `vendor/o3de` must have the gem export patch in
`o3de/patches/` applied; build with `-DGZ_O3DE_INTEROP=OFF` for a patch-free
backend (the interop probe / GL self-test are then logged no-ops).

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
| Atom native handle access | ⚠️ declared, not linkably reachable | `RHI.Interface/Vulkan/RHIVulkanInterface.h` declares `GetDeviceNativeHandle` / `GetNativeImage` / `GetImageMemory` / `GetFenceNativeHandle`, but the definitions are local (`t`) in the gem `.so` and only `T` in a unity static archive — see Step 1b for why reaching them needs either a gem export patch (A) or a risky static-archive link (B) |
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

1. **Foundation (verifiable headlessly) — DONE.** Connect the
   `ExternalHandleRequirementBus` + `DeviceRequirementBus` handlers before
   `Start()`; create a **persistent** exportable `AttachmentImage` (the RTT pass
   "Output" attachment is *transient* — `AttachmentLifetimeType::Transient`,
   allocated per-frame from the transient pool, so its `VkImage`/memory are not a
   stable handle to share); fetch its per-device `DeviceImage`, call
   `GetImageMemory` + `vkGetMemoryFdKHR`, assert a valid FD. Proves the foundation
   **without any GL or display** (FD ≥ 0 is the pass/fail signal). ✅ verified.
2. **GL import — DONE.** On a GL context, import the FD to a GL texture
   (`glCreateMemoryObjectsEXT` + `glImportMemoryFdEXT` with the **whole VMA block**
   `allocationSize`, then `glTextureStorageMem2DEXT` at the image's
   `allocationOffset`, `GL_OPTIMAL_TILING_EXT`); read the texels back and compare.
   ✅ verified headlessly (`GZ_O3DE_INTEROP_GLTEST=1`, `O3deGlInterop.cc`): a known
   gradient uploaded into the exportable image read back **bit-exact via GL** (all
   65536 texels). Key results: VMA sub-allocates (FD is the whole block, image at a
   non-zero offset — handled); optimal tiling imports correctly; **no semaphore was
   needed for this *static* image** (uploaded once, then read). `glImportMemoryFdEXT`
   takes ownership of the FD. Concurrent render→read still needs sync (step 3).
2.5. **Render-into (next):** the proof used a CPU-uploaded gradient. For the live
   path the *scene* must land in the shared persistent image — the RTT pass output
   is transient, so either swap the pipeline to a template with an output slot and
   use `CreateRenderPipelineForImage`, or add a copy/blit from the RTT output into
   the persistent `AttachmentImage` each frame. Verify by reading the persistent
   image back (existing tooling) and matching the readback frame.
3. **Semaphore sync:** export Atom's frame semaphore (the `ExternalHandleRequirement`
   semaphore collector is already wired), import to GL, add `glWaitSemaphoreEXT`/
   `glSignalSemaphoreEXT` (with `srcLayouts`) around the sample. Required once the
   image is written every frame concurrently with GL sampling.
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

**Step 1b — DONE & verified (FD export proved).** With `GZ_O3DE_INTEROP=1` the
backend now creates a persistent, exportable 256×256 `AttachmentImage`, pulls its
`VkDeviceMemory` via the patched gem accessors, and `vkGetMemoryFdKHR` returns a
valid FD (observed `fd=136`, image VMA-suballocated in a 32 MB block at
`offset=9083904`). Exit clean, zero Vulkan/AZ errors, readback path untouched. The
log line is `interop: PROVED FD export -- vkGetMemoryFdKHR returned fd=...`. This
proves the full exportable-image foundation: bus-enabled exportable VMA memory +
the device extension + cross-module access to the native handle. Two findings drove
the final shape (the "inline the thin casts" plan, option 1 below, was disproven):

**Finding 1 — the accessors are only reachable by patching the gem (chose A).**
Symbol audit of `vendor/o3de/build/linux` (NVIDIA profile build):

* The leaf accessors `AZ::Vulkan::Image::GetNativeImage()`,
  `Image::GetMemoryView()`, `Device::GetNativeDevice()` are **out-of-line** (only
  `MemoryView::GetNativeDeviceMemory()` is inline). So a `static_cast` written in
  `O3deBackend.cc` still needs those symbols at link time — inlining the *cast*
  does not inline the *accessor*.
* In the loaded runtime gem `libAtom_RHI_Vulkan.Private.so` those symbols are
  **local** (`nm` shows `t`, lowercase) — the `.so` exports **zero** dynamic
  `AZ::Vulkan` symbols (4 dynamic text symbols total: the gem entry points). They
  are therefore **not resolvable at runtime** from another module.
* They are defined (`T`) only in the **static** archive
  `libAtom_RHI_Vulkan.Private.Static.a`. That archive is a **unity build** — 11
  giant `unity_N_cxx.cxx.o` blobs. `GetNativeImage` lives in `unity_3_cxx.cxx.o`
  (3.5 MB), which bundles **9+ Vulkan-RHI classes** (Image, ImagePool,
  FrameGraphExecuter, …), carries **4 global constructors** (`.init_array`), and
  references **`AZ::Environment::GetInstance()` / EnvironmentVariable registration**.
  There is **no clean leaf object** to extract.
* The public free functions (`RHIVulkanInterface.cpp`:
  `AZ::Vulkan::GetNativeImage(RHI::DeviceImage&)`, `GetImageMemory`,
  `GetDeviceNativeHandle`) live in the `.Interface` target, which **was not built**
  (no `.Interface.a` exists) and PUBLIC-depends on `.Private.Static` anyway — same
  wall.

So the only way to call these accessors is to have the calling code compiled
*into* the Vulkan-RHI module, or to link the unity static archive — and linking it
runs registration-bearing global constructors that **double-register
`AZ::Environment` variables** against the already-loaded gem `.so`. Option 1
(inline casts) is therefore **not viable**. **Option A** was chosen and applied:
`o3de/patches/0001-export-vulkan-native-handle-accessors.patch` adds
`RHIVulkanInterface.cpp` to the **`.Private` gem MODULE** file list and wraps
`RHIVulkanInterface.h`'s declarations in `#pragma GCC visibility push(default)`, so
all 14 accessors export as dynamic `T` from the single loaded
`libAtom_RHI_Vulkan.Private.so` (verified). The plugin links that `.so`
(`DT_NEEDED`) to resolve them; AZ's own loader uses `RTLD_NOLOAD` and explicitly
tolerates the gem being "already loaded as a dependency" (no double-init). No
static archive, no duplicate state — verified `0` asserts at runtime.

**Finding 2 — exportable memory is necessary but not sufficient; the device
extension must also be enabled.** Even with the `ExternalHandleRequirementBus`
handler making VMA memory exportable, `vkGetMemoryFdKHR` first returned *unresolved*
because O3DE never enables the `VK_KHR_external_memory_fd` **device extension** (it
only uses the semaphore-FD extension; `external_memory_fd` is not in its
`OptionalDeviceExtension` enum). `vkGetDeviceProcAddr` returns null for an entry
point of a non-enabled extension. Fix (no patch): `GzExternalHandleProvider` also
handles the **`DeviceRequirementBus`** (same `VulkanBus.h`) and adds
`VK_KHR_external_memory` + `VK_KHR_external_memory_fd` via
`CollectAdditionalRequiredDeviceExtensions`, which `Device::GetRequiredExtensions()`
broadcasts at device creation.

**Build gating.** All of step 1b is behind the CMake option **`GZ_O3DE_INTEROP`**
(default **`ON`**). When ON, `O3deBackend.cc` is compiled with
`GZ_O3DE_INTEROP_BUILD`, links the gem `.so`, and requires the gem patch (else the
plugin link fails with undefined `AZ::Vulkan::Get*`, a clear signal to apply the
patch). Turn it **`OFF`** for a patch-free build: the plugin then compiles without
the accessor calls and does **not** link the gem `.so` (verified: no `NEEDED`
Vulkan entry, no accessor references), and the interop FD probe is a logged no-op.

<details><summary>Original Finding-1 evidence (kept for the record)</summary>

The chosen "inline the thin casts" approach (option 1 below) was investigated to
ground and **disproven**; the linkage requirement is unavoidable. Hard evidence
(symbol audit of `vendor/o3de/build/linux`, NVIDIA profile build):

* The leaf accessors `AZ::Vulkan::Image::GetNativeImage()`,
  `Image::GetMemoryView()`, `Device::GetNativeDevice()` are **out-of-line** (only
  `MemoryView::GetNativeDeviceMemory()` is inline). So a `static_cast` written in
  `O3deBackend.cc` still needs those symbols at link time — inlining the *cast*
  does not inline the *accessor*.
* In the loaded runtime gem `libAtom_RHI_Vulkan.Private.so` those symbols are
  **local** (`nm` shows `t`, lowercase) — the `.so` exports **zero** dynamic
  `AZ::Vulkan` symbols (4 dynamic text symbols total: the gem entry points). They
  are therefore **not resolvable at runtime** from another module.
* They are defined (`T`) only in the **static** archive
  `libAtom_RHI_Vulkan.Private.Static.a`. That archive is a **unity build** — 11
  giant `unity_N_cxx.cxx.o` blobs. `GetNativeImage` lives in `unity_3_cxx.cxx.o`
  (3.5 MB), which bundles **9+ Vulkan-RHI classes** (Image, ImagePool,
  FrameGraphExecuter, …), carries **4 global constructors** (`.init_array`), and
  references **`AZ::Environment::GetInstance()` / EnvironmentVariable registration**.
  There is **no clean leaf object** to extract.
* The public free functions (`RHIVulkanInterface.cpp`:
  `AZ::Vulkan::GetNativeImage(RHI::DeviceImage&)`, `GetImageMemory`,
  `GetDeviceNativeHandle`) live in the `.Interface` target, which **was not built**
  (no `.Interface.a` exists) and PUBLIC-depends on `.Private.Static` anyway — same
  wall.

**Conclusion:** the only way to call these accessors is to have the calling code
compiled *into* the Vulkan-RHI module, or to link the unity static archive — and
linking it pulls registration-bearing blobs whose 4 global constructors would run
at plugin load and **double-register `AZ::Environment` variables** against the
already-loaded gem `.so` (corrupt/duplicate cross-module singleton state). This is
the "it gets gnarly" stop condition; option 1 is **not viable as specified**.

Remaining candidate approaches (re-ranked by the evidence):
  A. **Tiny export patch to the Vulkan-RHI gem (recommended).** Add
     `Source/RHI.Interface/RHIVulkanInterface.cpp` to the **`.Private` gem MODULE**
     file list (so it compiles into the *one* loaded `libAtom_RHI_Vulkan.Private.so`)
     and give its 4 free functions **default visibility** (export attribute). The
     plugin then declares the prototypes from `RHIVulkanInterface.h` and calls them
     — resolved dynamically against the single loaded gem `.so`. No duplicate code,
     no extra static initializers, one module. **Cost:** a minimal patch to the
     vendored O3DE Vulkan gem + rebuild of that one gem — i.e. it **breaks the
     "no O3DE fork" property** the rest of M4 relied on.
  B. **Link `.Private.Static` into the plugin and verify empirically.** No O3DE
     change, faithful to "do it in the plugin", but pulls ~the whole 39 MB unity
     archive and runs its global constructors → confirmed double-registration
     hazard. Would need a gated build + run of the verified demo to see whether AZ
     asserts; high risk to the working PoC, and likely to fail.
  C. **Pause M4 step 1b.** Step 1a already proved the *foundation* (Atom creates
     the RTT image + VMA memory exportable once our bus handler is connected). Treat
     the live FD-export proof as future work pending the A-vs-B linkage decision;
     the PoC's verified readback path is unaffected.

Recommendation: **A** (clean, ~10-line gem patch) if a small O3DE-side patch is
acceptable; otherwise **C** until that constraint is revisited. **B** only as a
throwaway experiment, never as the shipping path.

</details>

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
