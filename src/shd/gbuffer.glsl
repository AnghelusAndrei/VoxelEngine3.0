// Shared gbuffer pixel format (RGBA32UI, full-res). Single source of truth for
// primary.comp (writer) and resolve.comp / later per-voxel passes (readers).
//
//   x : pos.x[0:15] | pos.y[16:31]
//   y : pos.z[0:15] | sizeLevel[16:23] | version[24:31]
//   z : material[0:6]
//   w : vid - octree node id of the hit (dedup TAS in downscale.comp, and the
//       baked-slot-offset lookup in resolve.comp). Meaningless on a miss (level==0).
//
// `sizeLevel` is the octree depth of the hit (root = 0, finest = octreeDepth), so
// every real hit has sizeLevel >= 1. A miss writes sizeLevel == 0 (w may carry steps).

struct GBuffer { bool hit; uvec3 pos; uint level; uint material; uint version; uint vid; };

uvec4 PackGBuffer(uvec3 pos, uint level, uint material, uint version, uint vid) {
    return uvec4(
        (pos.x & 0xFFFFu) | (pos.y << 16u),
        (pos.z & 0xFFFFu) | ((level & 0xFFu) << 16u) | ((version & 0xFFu) << 24u),
        material & MATERIAL_MASK,
        vid
    );
}

// Ancestor cache key: the same .xy layout, LB_ANCESTOR_FLAG in the level byte, version 0.
// Internal-node versions are never maintained (only makeLeaf bumps one), so the key is a pure
// function of (cell, level): anything holding a position can find the ancestor by hash, with no
// descent. The flag also keeps D1 non-zero for the root (level 0).
uvec2 PackAncestorKey(uvec3 cell, uint level) {
    return PackGBuffer(cell, level | LB_ANCESTOR_FLAG, 0u, 0u, 0u).xy;
}

// ---- virtual gbuffer only ------------------------------------------------------------
// `downscale` writes a plain copy of a full-res texel. `shade` then stamps this frame's
// uniqueVoxelList index into .z's spare bits - material needs only 7 of 32 - once its own
// lBuffer lookup has resolved it. `accum` reads it straight back, so it performs no slot
// lookup, no hash and no octree read at all.
//
//   z : material[0:6] | listIndex[7:31]
//
// 22 bits caps the virtual framebuffer at 4.19 M pixels (Renderer checks this on resize) -
// only reachable at virtualScale 1 above ~2K. The material field bought those 3 bits.
// VG_LISTIDX_NONE means "no accumulator": sky, or a voxel that never seated this frame.
#define VG_LISTIDX_SHIFT MATERIAL_BITS
#define VG_LISTIDX_NONE  0x3FFFFFu   // 22 bits

uint  VGListIdx(uvec4 g) { return g.z >> VG_LISTIDX_SHIFT; }
uvec4 VGSetListIdx(uvec4 g, uint idx) {
    return uvec4(g.x, g.y, (g.z & MATERIAL_MASK) | (idx << VG_LISTIDX_SHIFT), g.w);
}

GBuffer UnpackGBuffer(uvec4 g) {
    GBuffer o;
    o.level    = (g.y >> 16u) & 0xFFu;
    o.hit      = (o.level != 0u);
    o.pos      = uvec3(g.x & 0xFFFFu, g.x >> 16u, g.y & 0xFFFFu);
    o.version  = (g.y >> 24u) & 0xFFu;
    o.material = g.z & MATERIAL_MASK;
    o.vid    = g.w;
    return o;
}
