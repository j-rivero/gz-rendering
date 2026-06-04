# pbr_materials — real gz materials on the o3de backend (M12)

Builds a scene 100% through the public gz-rendering API (no demo
injection): a metallic roughness sweep (0.05→0.95), a gold sphere, a
sphere with a real albedo texture file (`Material::SetTexture`), a
ground box and two point lights. Renders headless via the CPU-readback
path and writes a PNG.

## Build

```bash
source /home/jrivero/code/gz/ws_o3de_rendering/install/setup.bash
cmake -S . -B build
cmake --build build -j5
```

## Run

```bash
./pbr_materials.sh                 # writes /tmp/m12_pbr_materials.png
./pbr_materials.sh out.png         # custom output path
```

Expect: bottom row of 5 chrome-like spheres going mirror→dull left to
right, a gold sphere and a "gz-rendering"-textured sphere floating above,
all PBR-lit on a grey ground. Compare with
`../../docs/screenshots/m12-pbr-materials-gz-api.png`.
