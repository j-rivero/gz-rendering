# mesh_pbr_viewer (M13)

Loads any mesh file gz-common can parse (GLB, .dae, .obj, ...) through the
public gz-rendering API on the o3de backend and renders it headless to a
PNG — **without calling SetMaterial**, so what you see is the material the
mesh FILE carries, auto-applied by the backend (M13): diffuse +
metallic/roughness factors and the albedo / normal / metalness / roughness
texture maps (GLB embedded textures and .dae sidecar texture files both
work).

The camera auto-frames from the mesh bounds; a ground box and key/fill
point lights (scaled to the model extent) are added with explicit gz
materials — those exercise the precedence rule (explicit gz material wins
over any file material).

## Build

    cd o3de/examples/mesh_pbr_viewer
    mkdir -p build && cd build
    source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash
    cmake .. && make

## Run

    ./mesh_pbr_viewer.sh <mesh-file> [out.png]

Example (jetty warehouse forklift, GLB with embedded PBR texture set):

    ./mesh_pbr_viewer.sh \
      /home/jrivero/code/gz/jetty_demo/jetty_demo/models/Forklift/base_visual.glb \
      /tmp/forklift.png

Look for the backend telemetry line confirming the file material applied:

    [gz-o3de] M13 file-material id=... albedo=mem normal=mem metal=mem rough=mem factors m=... r=...

(`mem` = in-memory/embedded texture, `path` = sidecar file, `none` = map not
present, `fail` = decode/upload failed.)
