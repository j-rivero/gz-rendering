# O3DE patches (interop only)

These patches apply to the **vendored O3DE engine tree** (`vendor/o3de`), not to
gz-rendering. They are needed **only** for the experimental M4 zero-copy
Vulkan→GL interop path (`-DGZ_O3DE_INTEROP=ON`). The base PoC
(`GZ_O3DE_INTEROP=OFF`, the default) builds and runs against an unpatched O3DE.

## `0001-export-vulkan-native-handle-accessors.patch`

**What.** Exports the Vulkan-RHI native-handle accessors
(`AZ::Vulkan::GetNativeImage` / `GetImageMemory` / `GetDeviceNativeHandle` / …)
from the loaded gem shared object `libAtom_RHI_Vulkan.Private.so`. It:

1. wraps the declarations in `RHIVulkanInterface.h` with
   `#pragma GCC visibility push(default)` (the gem is built `-fvisibility=hidden`,
   which otherwise keeps these symbols local to the `.so`), and
2. adds `Source/RHI.Interface/RHIVulkanInterface.cpp` to the `.Private` gem
   module's file list so the accessors are compiled **into** that one loaded
   `.so` (they were previously only in the unbuilt `.Interface` static target).

**Why.** The gz-rendering O3DE plugin is a separate loaded module. It needs the
`VkDeviceMemory` of an Atom image to call `vkGetMemoryFdKHR`. The accessors are
out-of-line and, unpatched, are *local* symbols in the gem `.so` — unreachable
from the plugin. The alternative (linking the gem's 39 MB unity static archive
into the plugin) pulls registration-bearing object blobs whose global
constructors would double-register `AZ::Environment` state against the
already-loaded gem. This patch keeps everything in the single gem module: no
duplicate code, no duplicate static state. See `../M4_INTEROP_DESIGN.md` (Step 1b)
for the full symbol-audit rationale.

This does **not** enable the `VK_KHR_external_memory_fd` device extension — that is
done at runtime from the plugin via the `DeviceRequirementBus` handler (no patch).

## Applying

```bash
# From the gz-rendering source root, against the pinned O3DE checkout:
git -C vendor/o3de apply o3de/patches/0001-export-vulkan-native-handle-accessors.patch

# Rebuild only the affected gem (profile config; ninja on PATH):
cmake --build vendor/o3de/build/linux --config profile -j5 \
  --target Atom_RHI_Vulkan.Private

# Verify the accessors are now exported (should print T symbols):
nm -C -D vendor/o3de/build/linux/bin/profile/libAtom_RHI_Vulkan.Private.so \
  | grep 'AZ::Vulkan::GetNativeImage'
```

Then configure gz-rendering with `-DBUILD_O3DE=ON -DGZ_O3DE_INTEROP=ON`. If the
patch is missing, the plugin link fails with undefined `AZ::Vulkan::Get*`
references — that is the signal to apply it.
