#pragma once

#include "./renderer/octree.hpp"
#include "./renderer/material.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Scene import — END-USER LAYER, above the renderer.
//
// A model file becomes voxels in the master octree and then stops existing:
// there is no object list, no transform, no bake/unbake, no scene graph. Import
// is content authoring, exactly like the procedural insertBox/insertSphere calls
// it sits next to in VoxelEngine — the octree remains the sole runtime truth and
// the renderer never learns that a file was involved.
//
// Two steps, deliberately separable:
//   load()  — file -> a lightweight Model: TRIANGLES in lattice coords (a mesh) or
//             a small authored grid (a .vox), plus the local material palette.
//             Pure CPU, zero GL. There is NO dense voxel grid for meshes — the
//             octree is the sparse structure that exists to avoid one, so a mesh
//             rasterizes STRAIGHT into it at place(). (A depth-11 building's dense
//             grid was ~2 GB, larger than the octree it produced.)
//   place() — Model -> Octree::insert + MaterialPool. For a mesh this is the
//             voxelizer: it rasterizes each triangle directly into the octree.
// import() is the one-liner that does both.
// ---------------------------------------------------------------------------

namespace scene {

constexpr int DEFAULT_RESOLUTION = 64;
constexpr size_t MAX_PALETTE = MATERIAL_MAX - 1;

// One triangle, vertices in LATTICE coords once load() has scaled them. `mat`
// indexes the Model palette. Public because a mesh Model carries its triangles
// (place() rasterizes them) rather than a voxel grid.
struct Tri {
    glm::vec3 v[3];
    glm::vec2 uv[3];    // (0,0) when the .obj carries no vt for this face
    uint8_t   mat;      // index into the model's SOURCE materials, not into `palette`
};

// A decoded diffuse / alpha map, downsampled on load. Voxel-scale sampling does
// not need the source resolution, and 54 full-size Sponza maps would be ~700 MB.
struct Texture {
    int                  w = 0, h = 0;
    std::vector<uint8_t> rgba;          // w*h*4
    bool ok() const { return w > 0 && h > 0; }
    glm::vec4 sample(glm::vec2 uv) const;
};

// Format-agnostic voxelization product. A mesh populates `tris`; a .vox populates
// `cells`; exactly one is non-empty. `dim` is the lattice AABB extent either way,
// used to centre the model at place().
struct Model {
    glm::ivec3               dim = glm::ivec3(0); // lattice AABB extent
    std::vector<Tri>         tris;                // MESH: triangles in lattice coords
    std::vector<uint8_t>     cells;               // .vox: 0 = empty, else palette index + 1
    std::vector<Material>    palette;             // engine materials, not yet pooled
    std::vector<std::string> paletteNames;        // parallel to palette; .mtl name / "vox<i>"

    // ---- textured source materials (meshes) -------------------------------------
    // A real .obj keeps its colour in TEXTURES, not in Kd: every one of Sponza's 25
    // materials declares `Kd 1 1 1` and differs only by `map_Kd`. Ignoring the maps
    // renders the whole scene flat white, so a source material with a diffuse map is
    // expanded at load() into up to `colorsPerMaterial` palette entries — the
    // dominant colours of its texture — and place() picks among them per voxel by
    // sampling the map at the voxel's interpolated UV.
    //
    // Expanding at LOAD, not at place(), is deliberate: place() must be able to bind
    // the whole palette before writing a single voxel, so an import never half-lands.
    //
    // `Tri::mat` indexes these; `srcSlot[m] .. srcSlot[m]+srcCount[m]` is that source
    // material's run inside `palette`.
    std::vector<int>     srcSlot;                 // per source material: first palette entry
    std::vector<int>     srcCount;                // per source material: entries (>= 1)
    std::vector<int>     srcTex;                  // per source material: index into `textures`, -1 = none
    std::vector<int>     srcMask;                 // per source material: map_d index, -1 = none
    std::vector<Texture> textures;                // decoded, deduplicated by path
    int                      thickness = 0;       // min surface thickness (meshes: applied at place)
    std::string              source;              // path as given to load()
    std::string              error;               // non-empty => load failed

    bool ok() const { return error.empty() && !(tris.empty() && cells.empty()); }

    // .vox only (a mesh has no grid). Undefined if `cells` is empty.
    uint8_t at(int x, int y, int z) const {
        return cells[(size_t)x + (size_t)dim.x * ((size_t)y + (size_t)dim.y * (size_t)z)];
    }

    // Palette slot named `name`, or -1. Real .obj scenes carry their meaning in
    // material NAMES ("floor", "column_a", "fabric_a") while every Kd may be an
    // identical placeholder because the colour lived in a texture — so authoring
    // by name is the only workable way to give such a model materials.
    int slot(const std::string& name) const {
        for (size_t i = 0; i < paletteNames.size(); i++)
            if (paletteNames[i] == name) return (int)i;
        return -1;
    }

    // The source material a palette entry belongs to, or -1. A textured material
    // owns a RUN of entries (its texture's dominant colours), so anything that
    // restyles by name has to act on the run, not on one slot.
    int runOf(int paletteIndex) const {
        for (size_t m = 0; m < srcSlot.size(); m++)
            if (paletteIndex >= srcSlot[m] && paletteIndex < srcSlot[m] + srcCount[m])
                return (int)m;
        return -1;
    }

    // Restyle one palette slot in place, keeping its name and its voxels.
    // Optional: an import is fully materialled without ever calling this — use it
    // when an asset's own .mtl is uninformative (Sponza ships one placeholder
    // grey for all 25 of its materials). Returns false if `name` is not in the
    // palette, so a renamed asset degrades to "unstyled", never to a wrong slot.
    bool paint(const std::string& name, glm::vec3 albedo, float rough,
               float spec, float metal, float emit = 0.0f) {
        const int i = slot(name);
        if (i < 0) return false;
        const int m   = runOf(i);
        const int beg = (m >= 0) ? srcSlot[(size_t)m]  : i;
        const int cnt = (m >= 0) ? srcCount[(size_t)m] : 1;
        for (int k = beg; k < beg + cnt; k++) {
            Material& p = palette[(size_t)k];
            p.color             = glm::vec4(albedo, 0.0f);   // flattens the texture run
            p.roughness         = rough;
            p.specular          = spec;
            p.metallic          = metal;
            p.emissive          = (emit > 0.0f) ? 1 : 0;
            p.emissiveIntensity = emit;
        }
        return true;
    }

    // Keep a textured material's colours, restyle only its surface response. This is
    // usually what you want on an imported asset: the texture already knows the
    // albedo, the .mtl rarely knows the roughness.
    bool finish(const std::string& name, float rough, float spec, float metal) {
        const int i = slot(name);
        if (i < 0) return false;
        const int m   = runOf(i);
        const int beg = (m >= 0) ? srcSlot[(size_t)m]  : i;
        const int cnt = (m >= 0) ? srcCount[(size_t)m] : 1;
        for (int k = beg; k < beg + cnt; k++) {
            palette[(size_t)k].roughness = rough;
            palette[(size_t)k].specular  = spec;
            palette[(size_t)k].metallic  = metal;
        }
        return true;
    }
};

// Parse and voxelize. Supported: .obj (+ its .mtl) and .vox (MagicaVoxel).
// Never throws; on failure the returned Model has `error` set and ok() == false.
Model load(const std::string& path, int resolution = 0, bool solid = false,
           int thickness = 0, int colorsPerMaterial = 16);
uint64_t place(const Model& m, Octree& octree, MaterialPool& pool, glm::ivec3 origin);
uint64_t import(const std::string& path, Octree& octree, MaterialPool& pool,
                glm::ivec3 centre, int resolution = 0, bool solid = false,
                int thickness = 0, int colorsPerMaterial = 16);

} // namespace scene
