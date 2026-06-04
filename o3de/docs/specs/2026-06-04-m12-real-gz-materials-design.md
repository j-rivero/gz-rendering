# M12 — Real gz materials on runtime meshes (design)

Date: 2026-06-04
Status: approved
Branch: `o3de_render_engine_poc`

## Goal

Feed the now-proven Atom PBR pipeline (M11: diffuse color, metallic,
roughness, albedo texture file, IBL) from **real gz-rendering materials**
created through the public API, instead of the backend's hardcoded demo
injection. This is the first milestone where scene content + materials are
100% gz-sourced.

## Context

- `O3deScene::CreateMeshImpl` already registers real geometry with the
  backend (M9-B) and `O3deRenderTarget::GatherFrame` already snapshots
  `Material()->Diffuse()` into `O3deMeshData.color`.
- `O3deMeshData` already carries `metallic`, `roughness`, `texturePath`
  (M11) — but only the backend's demo injection ever sets them.
- `O3deMaterial` stores texture path / roughness / metalness itself:
  `BaseMaterial`'s accessors for these are no-ops (only color and common
  flags are stored there). Diffuse is stored by `BaseMaterial`. Nothing
  Atom-specific is needed on the gz wrapper side.
- All current demo content is backend-injected (`MaybeInjectDemoShapes`),
  bypassing the gz scene graph; the gz scene in the live demo is empty.

## Decisions (approved 2026-06-04)

| Question | Decision |
|---|---|
| Driver / verification vehicle | Standalone example under `o3de/examples/pbr_materials/` (not gz-gui, not gz-sim) |
| Property scope | Proven set only: diffuse, metalness, roughness, albedo texture path |
| Geometry scope | Mesh geometries only; primitives stay AuxGeom (flat color) |
| Where properties cross to the backend | Approach A — extend the existing `GatherFrame` per-frame snapshot (no material registry, no push-on-set) |

## Part 1 — Wiring (`o3de/src/O3deRenderTarget.cc`)

In `GatherFrame`'s mesh branch (the `if (auto o3deMesh = ...)` block),
extend the existing `if (MaterialPtr mat = o3deMesh->Material())` body:

- `md.metallic = mat->Metalness();` and `md.roughness = mat->Roughness();`
  — unconditionally when a material is attached. gz defaults are
  legitimate values; the `-1` "leave Atom default" sentinels remain only
  for material-less meshes.
- `md.texturePath = mat->Texture();` when non-empty. The backend's
  `FileBaseColorImage()` (M11-D) decodes via `gz::common::Image` and
  caches per path — unchanged.
- `md.color` from `Diffuse()` — already present.

No backend (`O3deBackend.cc`) changes. No `O3deMaterial.cc` changes;
update its class comment to say properties are consumed via the frame
snapshot in `GatherFrame`.

## Part 2 — Driver example (`o3de/examples/pbr_materials/`)

`main.cc` — loads the `o3de` engine via `rendering::engine()` and builds
the scene entirely through the public gz-rendering API:

- Root visual → child visuals, each with
  `CreateMesh(MeshDescriptor("unit_sphere"))` (gz-common MeshManager
  built-in) and a `scene->CreateMaterial()` configured via
  `SetDiffuse` / `SetMetalness` / `SetRoughness` / `SetTexture`:
  - 5-sphere roughness sweep (0.05 → 0.95, metallic 1.0) — mirrors the
    proven M11-A demo scene for direct visual comparison
  - 1 gold metal sphere
  - 1 textured sphere: `SetTexture(<abs path>/gz_albedo_demo.png)`
    (reuses the existing demo asset)
- Ground: a flattened `unit_box` mesh with a rough dielectric material.
- Lights via the gz API: a key + fill point-light pair using the same
  values as the backend's `GZ_O3DE_DEMO_PBR_ONLY` lighting, so the
  capture compares 1:1 against the M11 reference screenshots.
- Camera: `scene->CreateCamera()`, ~1280×720,
  `camera->Capture(image)` → save PNG via `gz::common::Image`. This is
  the M0/M2 CPU-readback path — no Qt, no interop env vars, no QSG
  grey-race. Capture a few frames (loop) before saving the final one to
  ride out Atom's ~3 s boot/warmup.

`pbr_materials.sh` launcher — mirrors `live_demo.sh`: points
`GZ_RENDERING_PLUGIN_PATH` + `GZ_CONFIG_PATH` at the colcon install,
sets `GZ_O3DE_DEMO_IBL=1` (metals need IBL to read as metal), leaves
`GZ_O3DE_DEMO_SHAPES` unset so demo injection is naturally off.

CMake: `add_executable` following the repo's existing example pattern.

## Error handling

- Mesh without material → sentinels stay `-1` / `texturePath` empty →
  backend defaults, today's behavior.
- Material without texture → `Texture()` empty → checker/none, as today.
- Bad texture path → backend's existing decode-failure log + fallback.

## Verification

1. Build (`make gz-rendering-o3de` in `build_o3de`, install over BOTH
   workspace .so copies), run the example, inspect the saved PNG against
   the M11 references (`o3de/docs/screenshots/m11-pbr-*.png`): roughness
   sweep gradient visible, gold reads gold, texture text wraps the sphere.
2. Telemetry in the log: `RegisterMesh` per mesh, texture decode line for
   the file path, no demo-injection lines.
3. Save the result capture to `o3de/docs/screenshots/m12-*.png`.

## Out of scope (stated limitations)

- Per-submesh materials — `O3deSubMesh::SetMaterialImpl` stays no-op.
- Primitives (box/sphere/cylinder/plane geometries) stay AuxGeom.
- Emissive, normal map, specular, transparency, environment map —
  unproven Atom-side, future milestone.
- Property changes mid-run are picked up automatically (per-frame
  snapshot) but not explicitly exercised by the example.

## Docs / memory updates (handoff contract)

- Add the M12 row to `HANDOFF_FOR_NEXT_AGENT.md` milestones; update TL;DR.
- Update `~/.claude/memory/project_o3de_rendering_roadmap.md`.
