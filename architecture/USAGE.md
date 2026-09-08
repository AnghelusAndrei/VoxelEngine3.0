# Renderer Usage & Materials

How the engine is driven from the application side, and the material system it draws through.

Frame internals: [PIPELINE.md](PIPELINE.md). Node format: [OCTREE.md](OCTREE.md). Cache:
[LBUFFER.md](LBUFFER.md).

## Wiring

`VoxelEngine` owns five objects and hands them to `Renderer`, which owns no scene state of its own:

```cpp
octree       = new Octree(&octreeConfig);        // depth is fixed at construction
camera       = new FPCamera(&cameraConfig, &controllerConfig);
materialPool = new MaterialPool();
skybox       = new Skybox("./assets/skybox", …); // 6 faces
renderer     = new Renderer(&rendererConfig, octree, camera, materialPool, skybox);
```

`RendererConfig` is immutable for the session (log callback, framebuffer-size callback, aspect ratio,
debug flag). `FrameConfig` is per-frame and is what the UI mutates — render mode, `virtualScale`, the
EMA and staleness windows, `normalPrecision`, `primary_raystop`, and the `shaderRecompilation`
one-shot. The loop is just:

```cpp
while (!glfwWindowShouldClose(window)) {
    /* input, edits, UI */
    if (!renderer->run(&frameConfig)) break;     // false => a GL error was logged
    glfwSwapBuffers(window);
}
```

`renderer->run` does the whole frame ([PIPELINE.md](PIPELINE.md)) and writes `profiler` timings and
`stats` that the Info widget reads. Editing the octree mid-run is expected: `insert`/`remove` mutate
the CPU tree, `flushEdits()` uploads the dirty range, and `edit_mark.comp` re-flags the affected
neighbourhood so normals recompute. The renderer never learns *why* the tree changed.

Shaders hot-reload from `./shd` at runtime via the **Recompile shaders** button; a pass whose shader
fails to compile keeps its previous program, so a typo degrades one pass instead of the frame.

## The demo scene

Built in the `VoxelEngine` constructor, and it is content authoring, not engine structure — the octree
is the only runtime truth and nothing survives import as an object.

Sponza is imported through `scene::load` / `scene::place` (`src/scene.hpp`) at **depth 10**,
resolution 1000, thickness 3, taking its palette from its own textures — its `.mtl` declares
`Kd 1 1 1` for all 25 materials and keeps every colour in `map_Kd`, so the importer expands each
textured material into the dominant colours of its map (25 source materials → ~332 palette entries).
`thickness 3` is not cosmetic: `normal.comp` derives normals from an occupancy gradient, so a
one-voxel-thick sheet has no perpendicular component and falls back to a hardcoded up-vector — Sponza
is full of such curtains and leaves.

The model is floor-mounted rather than centred, one emissive sphere at the atrium centre is the only
light, and the camera starts inside looking down the long axis. Five `brush.*` materials back the
runtime editor.

**Controls:** left click carves a sphere, right click inserts one, `1`–`9` pick the brush material and
size, `Esc` toggles the UI and mouse capture.

---

## Materials

```cpp
struct Material {                 // 64 B exactly
    glm::vec4 color;              // albedo; .rgb, alpha unused
    glm::vec4 specularColor;
    float roughness;              // 0 = mirror, 1 = fully rough
    float specular;               // intensity; ~0.04 for dielectrics
    float metallic;
    GLint emissive;               // GLint, to match std140 bool layout
    float emissiveIntensity;
    float _pad[3];                // explicit tail padding
};
```

The padding is load-bearing: it makes std140 and std430 agree on the array stride, so the same struct
works whether the pool is a UBO or an SSBO. A `static_assert` pins `sizeof(Material) == 64`.

The pool lives in an **SSBO** (binding 8), not a uniform block. `MATERIAL_MAX` is 1024 — mirroring the
octree node's 10-bit material field — and 1024 × 64 B = 64 KB is the whole of a typical
`GL_MAX_UNIFORM_BLOCK_SIZE` and 4× the 16 KB the GL spec guarantees, so anything past 256 materials
cannot portably be a UBO.

**Slot 0 is the empty sentinel**, so 1023 are usable. `addMaterial` bounds-checks and returns 0 when
full — a visibly missing material rather than a silent out-of-range write.

### Names

Every material carries a name, and the pool is addressable by it (`findByName`, `nameOf`). Names are
made unique on insert: a second `"floor"` becomes `"floor (2)"`, Windows-style.

This exists because real assets carry their meaning in material **names** while their values may be
identical placeholders. Keying the pool by value would collapse `"floor"`, `"column_a"`, `"leaf"` and
`"fabric_a"` into one slot that nothing could address apart.

### `distance` and `collapse`

```cpp
static float distance(const Material& a, const Material& b);
uint32_t collapse(uint32_t target, std::vector<uint32_t>& remap,
                  std::vector<std::string>* lost = nullptr);
```

`distance` is a squared metric over colour, roughness, metallic and specular, with emissive intensity
compared as a *ratio* rather than a difference. **Emissive and non-emissive are infinitely far apart**
(`FLT_MAX`), so a light can never fold onto a wall.

`collapse` merges the nearest pairs until at most `target` remain, weighting each survivor by how many
originals it now stands for, then compacts the survivors to the front. It is O(n³) — a one-shot for a
pool that has actually filled, never a per-frame operation. `lost` collects the names that were merged
away.

**`remap` must be applied to the geometry**, and this is the part that is easy to get wrong: voxels
already in the octree reference the *old* slot ids, so compacting the pool without remapping silently
repaints the entire scene. The full sequence is:

```cpp
if (pool.collapse(target, remap, &lost) > 0) {
    octree.remapMaterials(remap);   // rewrite every node's material, mark dirty
    pool.sync();                    // re-upload the compacted pool
}
```

`Octree::remapMaterials` walks the node pool and rewrites leaves *and* internal nodes — an internal
node's representative material is read by the DDA's coarse-hit fallback, so it has to move too.
