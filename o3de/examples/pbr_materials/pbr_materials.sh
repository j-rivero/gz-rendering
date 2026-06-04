#!/usr/bin/env bash
#
# M12: real gz materials -> Atom PBR through the public gz-rendering API.
#
# Renders a headless offscreen scene via the CPU-readback path (no Qt, no
# zero-copy interop) and writes a PNG. The scene -- a metallic roughness
# sweep, a gold sphere, a file-textured sphere, ground, two point lights --
# is built 100% with public gz API calls; GZ_O3DE_DEMO_SHAPES stays unset
# so the backend's demo injection is off.
#
# Usage: pbr_materials.sh [output.png]      (default /tmp/m12_pbr_materials.png)
#
# Env overrides (same as live_demo.sh):
#   GZ_O3DE_WS    colcon workspace whose install/ holds gz-rendering
#   GZ_O3DE_REPO  gz-rendering source checkout (for vendor/o3de runtime libs)

set -euo pipefail

GZ_O3DE_WS="${GZ_O3DE_WS:-/home/jrivero/code/gz/ws_o3de_rendering}"
GZ_O3DE_REPO="${GZ_O3DE_REPO:-/home/jrivero/code/gz/gz-rendering}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O3DE_LIBS="$GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile"

[ -f "$GZ_O3DE_WS/install/setup.bash" ] || {
  echo "ERROR: no colcon install at '$GZ_O3DE_WS/install'" >&2; exit 1; }
[ -d "$O3DE_LIBS" ] || {
  echo "ERROR: no vendored O3DE runtime libs at '$O3DE_LIBS'" >&2; exit 1; }
[ -x "$SCRIPT_DIR/build/pbr_materials" ] || {
  echo "ERROR: example not built -- see README.md" >&2; exit 1; }

# colcon's setup.bash references unbound vars; relax nounset for the source.
set +u
# shellcheck disable=SC1091
source "$GZ_O3DE_WS/install/setup.bash"
set -u
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$O3DE_LIBS"

# Metals need IBL to read as metal (M11-C). Readback path; no interop vars.
export GZ_O3DE_DEMO_IBL=1

TEX="$SCRIPT_DIR/../native_vulkan_live/assets/gz_albedo_demo.png"
OUT="${1:-/tmp/m12_pbr_materials.png}"

exec "$SCRIPT_DIR/build/pbr_materials" "$TEX" "$OUT"
