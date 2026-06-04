# M13 — Real-asset PBR: mesh-file materials + texture maps (design)

Date: 2026-06-04
Status: approved
Branch: `o3de_render_engine_poc`

## Goal

Make real-world assets (the jetty_demo warehouse models: GLB with embedded
PBR texture sets, plus textured .dae) render *nicely* on the o3de backend:
auto-apply the material a mesh file carries (factors + albedo / normal /
metalness / roughness maps) without any gz-API call, on top of the proven
M12 chain.

## Context (verified by smoke test, 2026-06-04)

- `/tmp/o3de_debug/glb_smoke/` proved the geometry path on the jetty
  Forklift GLB (15,777 verts / 46,476 indices): loads, converts, shades
  correctly — but renders GREY (`colour=0.8, metal/rough=-1` sentinels).
- gz-common's AssimpLoader extracts everything M13 needs into
  `common::Material`/`common::Pbr`: diffuse + metalness/roughness factors,
  and **in-memory** `common::Image` data for albedo, normal (TANGENT
  space), metalness and roughness maps — it even **splits the combined
  glTF metallic-roughness texture** (`SplitMetallicRoughnessMap`).
  File-based assets carry paths instead; both forms exist on the API.
- `SubMesh::GetMaterialIndex()` provides per-submesh material mapping.
- Atom StandardPBR has the needed properties:
  `{baseColor,normal,metallic,roughness}.{textureMap,useTexture}` plus the
  already-used `baseColor.color`, `metallic.factor`, `roughness.factor`
  (vendor `MaterialInputs/*PropertyGroup.json`).
- The backend already uploads CPU images to Atom
  (`CreateFromCpuData`, M11-B/D) with a per-path cache.

## Decisions (approved 2026-06-04)

| Question | Decision |
|---|---|
| Map scope | Core 4: albedo, normal, metalness, roughness. No emissive/light maps. |
| Multi-material meshes | Apply material[0] to the whole (flattened) mesh + one-shot warning. Per-submesh material slots are a future milestone. |
| Where materials cross to Atom | **Approach A**: backend-side, extracted once at `RegisterMesh` (matches the ogre2 precedent of engine-side mesh-material application). No gz-API changes, no snapshot bloat. |
| Precedence | An explicitly-set gz material (M12 snapshot with non-sentinel values) overrides the file material ENTIRELY (no mixing). Sentinels (no gz material attached) → file material applies. |
| Verification vehicle | Promote the glb_smoke scratch into `o3de/examples/mesh_pbr_viewer/` (mesh path argument, like the repo's mesh_viewer). |

## Part 1 — Extraction (gz thread, RegisterMesh time)

`O3deBackend::RegisterMesh(id, common::Mesh*)` gains a one-time extraction
of the mesh's material[0] into a plain struct stored with the registered
geometry record:

```cpp
struct MeshFileMaterialCpu
{
  bool present = false;
  float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};  // common::Material diffuse
  float metalness = 0.0f;                      // Pbr factor
  float roughness = 1.0f;                      // Pbr factor
  // In-memory images (GLB embedded textures; may be null):
  std::shared_ptr<const common::Image> albedoImg, normalImg,
                                       metalnessImg, roughnessImg;
  // File paths (e.g. .dae sidecar textures; may be empty):
  std::string albedoPath, normalPath, metalnessPath, roughnessPath;
};
```

Selection: the first submesh with a material index → that material; else
`MaterialByIndex(0)` when `MaterialCount() > 0`. If `MaterialCount() > 1`,
log a one-shot warning naming the mesh and the count.

Sources: `mat->Diffuse()`; `mat->PbrMaterial()` for factors and for each
map both the `*MapData()` (in-memory) and the map name/path forms.
Normal-map space: accept TANGENT (what assimp produces); any other space →
skip the normal map with a log line.

## Part 2 — Application (render thread, SubmitMeshes)

Per-mesh precedence, evaluated where the per-mesh material instance is
configured today:

- Snapshot carries explicit values (gz material attached: `metallic >= 0`
  / `roughness >= 0` / non-empty `texturePath`) → **explicit wins
  entirely**; the file material is ignored for that mesh.
- Snapshot carries the sentinels → apply `MeshFileMaterialCpu`:
  - `baseColor.color` ← color; `metallic.factor` ← metalness;
    `roughness.factor` ← roughness.
  - For each of the 4 maps, prefer the in-memory image, else the path:
    upload → set `<group>.textureMap` + `<group>.useTexture = true`.

Upload helper: generalize the M11-D image path into one function
`StreamingImageFromCommonImage(const common::Image &, bool _srgb)`:
- **sRGB** format for albedo; **UNORM (linear)** for normal, metalness,
  roughness — the classic washed-out-normals bug lives here, so the format
  choice is baked into the call sites.
- Cached: in-memory images keyed by `common::Image` data pointer + name;
  path-based reuse the existing per-path cache (extended with the srgb
  flag in the key).
- Metalness/roughness arrive as separate images (gz-common pre-splits the
  combined glTF map); each uploads as its own texture and StandardPBR
  samples the channel it needs.

Telemetry: one-shot per mesh, e.g.
`[gz-o3de] M13 file-material id=... albedo=mem normal=mem metal=mem rough=mem factors m=1.00 r=0.66`.

## Part 3 — Driver example (`o3de/examples/mesh_pbr_viewer/`)

The promoted glb_smoke scratch test:

- `mesh_pbr_viewer <mesh-file> [out.png]` — loads via
  `MeshDescriptor(path)` through the public gz API, NO `SetMaterial` (so
  the file material applies).
- Prints submesh/vertex/index/material counts + bounds (gz-common
  pre-load), auto-frames the camera from the bounds, ground box +
  key/fill point lights (intensity scaled by model extent), IBL on via
  the launcher (`GZ_O3DE_DEMO_IBL=1`).
- 30-frame warmup capture, zeroed buffer + all-zero failure guard
  (exit 1), PNG out, `fflush(stdout)`.
- `mesh_pbr_viewer.sh` launcher mirroring `pbr_materials.sh`; README.
- No binary assets shipped; the jetty models stay out-of-repo.

## Error handling

- Mesh without any material → `present=false` → today's grey defaults.
- Map decode/upload failure → log + skip that map; factors still apply.
- Map has neither image nor path → unwired, no error.
- Non-TANGENT normal-map space → skip normal map, log.
- `normal.factor` stays at the Atom default (no gz source for it).

## Verification

1. `mesh_pbr_viewer` on the jetty Forklift GLB → textured forklift
   (albedo + per-texel metal/rough + normal detail), vs the grey "before"
   (`/tmp/glb_smoke_forklift.png`, smoke test). Save
   `o3de/docs/screenshots/m13-forklift-textured.png` (+ optionally the
   grey before for contrast).
2. One `.dae` (e.g. a ur10 link) exercises the path-based texture form.
3. Telemetry shows the M13 file-material line with the expected sources.
4. Regression: re-run `pbr_materials` (explicit materials still win,
   image unchanged) and the live demo (demo meshes carry no file
   materials; unchanged).

## Out of scope (stated limitations)

- Per-submesh materials — material[0] applies to the whole flattened mesh.
- Emissive / light / occlusion maps.
- `O3deMesh::Material()` does NOT reflect the auto-applied file material
  (stays null unless explicitly set) — engine-side application, like ogre2.
- gz-API in-memory textures (`SetTexture(_texture, _img)`) still ignore
  `_img`; the file-path form + this auto-apply cover all current consumers.
- SDF `<pbr>` parsing — gz-sim's job, not the render engine's.

## Docs / memory updates (handoff contract)

- M13 row + TL;DR in `HANDOFF_FOR_NEXT_AGENT.md`.
- Update `~/.claude/memory/project_o3de_rendering_roadmap.md`.
