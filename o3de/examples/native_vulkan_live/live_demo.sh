#!/usr/bin/env bash
#
# Live demo: the experimental O3DE/Atom gz-rendering backend driving gz-gui's
# MinimalScene over NATIVE Vulkan->Vulkan zero-copy.
#
# What you should see: a grey viewport with three shaded primitives -- a red box
# (left), a green sphere (centre), and a blue cylinder (right) -- that you can
# orbit/pan/zoom with the mouse. Every frame is produced by Atom on its own
# VkDevice, exported as a Vulkan image (vkGetMemoryFdKHR), and imported + sampled
# directly by Qt's Vulkan RHI on its own VkDevice. No CPU readback, no copy.
#
# This is an experimental PoC. It needs:
#   * gz-rendering built with -DGZ_O3DE_INTEROP=ON (and the gem patch in
#     o3de/patches/), installed into the colcon workspace below;
#   * the Vulkan-interop gz-gui build in that same workspace (GZ_CONFIG_PATH
#     points at it so the patched MinimalScene runs, not the system one);
#   * a vendored O3DE build at $GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile.
#
# Paths default to the reference machine but can be overridden via the env:
#   GZ_O3DE_WS    colcon workspace whose install/ holds gz-rendering + gz-gui
#   GZ_O3DE_REPO  gz-rendering source checkout (for vendor/o3de runtime libs)
#
# Close the window (or Ctrl-C in this terminal) to stop.

set -euo pipefail

# --- locate the workspace + repo (override-able) ----------------------------
GZ_O3DE_WS="${GZ_O3DE_WS:-/home/jrivero/code/gz/ws_o3de_rendering}"
GZ_O3DE_REPO="${GZ_O3DE_REPO:-/home/jrivero/code/gz/gz-rendering}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="${GZ_O3DE_DEMO_CONFIG:-$SCRIPT_DIR/live_demo.config}"

O3DE_LIBS="$GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile"

# --- sanity checks (fail early with a clear message) ------------------------
[ -f "$GZ_O3DE_WS/install/setup.bash" ] || {
  echo "ERROR: no colcon install at '$GZ_O3DE_WS/install' -- build the workspace" >&2
  echo "       or set GZ_O3DE_WS to the right path." >&2
  exit 1
}
[ -f "$CONFIG" ] || { echo "ERROR: missing gz-gui config '$CONFIG'" >&2; exit 1; }
[ -d "$O3DE_LIBS" ] || {
  echo "ERROR: no vendored O3DE runtime libs at '$O3DE_LIBS'" >&2
  echo "       set GZ_O3DE_REPO to your gz-rendering checkout." >&2
  exit 1
}

# --- environment: patched gz-gui + Vulkan backend + interop toggles ---------
# colcon's setup.bash references unbound vars (e.g. COLCON_TRACE), so relax
# `nounset` just for the source, then restore it.
set +u
# shellcheck disable=SC1091
source "$GZ_O3DE_WS/install/setup.bash"
set -u
# Point gz at the workspace config so the Vulkan-interop gz-gui wins over system.
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$O3DE_LIBS"

# Force a 1.0 device pixel ratio on Qt's side. Without this, on a HiDPI
# display Qt 6.4.2 + QRhi-Vulkan + NVIDIA proprietary 580 renders the QSG
# scene graph (incl. our QSGSimpleTextureNode sampling Atom's imported
# VkImage) into an offscreen render target sized in LOGICAL pixels
# (Main.qml 1200x1000 minus the ToolBar header == 1200x902), but the swap-
# chain is created at PHYSICAL pixels (2048x1536 for DPR=2). The final
# composite/blit from that offscreen target into the swapchain never
# fires -- the swapchain stays at its `#fafafa` Material-Light clear and
# the on-screen viewport is uniformly grey, while the layer trace shows
# 141 fullscreen-triangle composite draws into the 1200x902 target and
# zero draws into any swapchain-targeting framebuffer.
#
# Forcing DPR=1 makes the logical size match the physical swapchain size
# so the implicit-render-target reuse path on this driver picks the swap-
# chain. The Qt 6.8 commit a15c3519 ("Optimize QQuickRt behavior with
# implicitly created buffers") restructures exactly this caching logic;
# upgrading the Qt build to >=6.8 should make this workaround unnecessary.
# Until then, both vars below are required (QT_SCALE_FACTOR alone is not
# enough on Qt 6.4.2 -- Qt re-derives a DPR from the screen unless the
# auto-scaling environment is also disabled).
export QT_SCALE_FACTOR=1
export QT_AUTO_SCREEN_SCALE_FACTOR=0

# Qt MinimalScene: use the Vulkan RHI backend (the importer side of zero-copy).
export GZ_GUI_RENDER_ENGINE_GUI_API_BACKEND=vulkan
# O3DE interop path: enabled, live (per-frame), with the render-finished
# timeline semaphore (#26) for cross-device frame sync.
export GZ_O3DE_INTEROP=1
export GZ_O3DE_INTEROP_LIVE=1
export GZ_O3DE_INTEROP_SEM=1
# Inject the box/sphere/cylinder when the scene is empty (stands in for gz-sim).
export GZ_O3DE_DEMO_SHAPES=1

echo "=============================================================="
echo " O3DE / Atom  ->  Qt   native Vulkan zero-copy  (LIVE)"
echo "--------------------------------------------------------------"
echo " workspace : $GZ_O3DE_WS"
echo " o3de libs : $O3DE_LIBS"
echo " config    : $CONFIG"
echo " expect    : red box | green sphere | blue cylinder, orbitable"
echo " close the window (or Ctrl-C) to stop."
echo "=============================================================="

exec gz gui -v 3 -c "$CONFIG"
