#!/usr/bin/env bash
#
# M13: view a real mesh asset with the material the FILE carries (GLB
# embedded PBR texture sets, .dae sidecar textures) -- the backend
# auto-applies it; the example makes no SetMaterial call.
#
# Renders a headless offscreen scene via the CPU-readback path (no Qt, no
# zero-copy interop) and writes a PNG.
#
# Usage: mesh_pbr_viewer.sh <mesh-file> [output.png]
#                                       (default /tmp/mesh_pbr_viewer.png)
#
# Env overrides (same as live_demo.sh):
#   GZ_O3DE_WS    colcon workspace whose install/ holds gz-rendering
#   GZ_O3DE_REPO  gz-rendering source checkout (for vendor/o3de runtime libs)

set -euo pipefail

[ $# -ge 1 ] || { echo "usage: mesh_pbr_viewer.sh <mesh-file> [out.png]" >&2; exit 2; }

GZ_O3DE_WS="${GZ_O3DE_WS:-/home/jrivero/code/gz/ws_o3de_rendering}"
GZ_O3DE_REPO="${GZ_O3DE_REPO:-/home/jrivero/code/gz/gz-rendering}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O3DE_LIBS="$GZ_O3DE_REPO/vendor/o3de/build/linux/bin/profile"

[ -f "$GZ_O3DE_WS/install/setup.bash" ] || {
  echo "ERROR: no colcon install at '$GZ_O3DE_WS/install'" >&2; exit 1; }
[ -d "$O3DE_LIBS" ] || {
  echo "ERROR: no vendored O3DE runtime libs at '$O3DE_LIBS'" >&2; exit 1; }
[ -x "$SCRIPT_DIR/build/mesh_pbr_viewer" ] || {
  echo "ERROR: example not built -- see README.md" >&2; exit 1; }

# colcon's setup.bash references unbound vars; relax nounset for the source.
set +u
# shellcheck disable=SC1091
source "$GZ_O3DE_WS/install/setup.bash"
set -u
export GZ_RENDERING_PLUGIN_PATH="$GZ_O3DE_WS/install/lib/gz-rendering/engine-plugins"
export GZ_CONFIG_PATH="$GZ_O3DE_WS/install/share/gz"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$O3DE_LIBS"

# Metals/speculars need IBL to read convincingly (M11-C).
export GZ_O3DE_DEMO_IBL=1

MESH="$1"
OUT="${2:-/tmp/mesh_pbr_viewer.png}"

[ -f "$MESH" ] || { echo "ERROR: mesh file not found: $MESH" >&2; exit 1; }

exec "$SCRIPT_DIR/build/mesh_pbr_viewer" "$MESH" "$OUT"
