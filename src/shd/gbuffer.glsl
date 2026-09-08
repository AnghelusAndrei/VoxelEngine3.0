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

// ---- holeFill confidence (holefill.comp -> resolve.comp) ---------------------------
#define HF_CONF_SHIFT 24u

// 1..255, never 0: a valid channel must always be able to contribute something, or a
// neighbourhood of noisy-but-valid taps would renormalize to nothing and go black.
float HFConfidence(uvec4 hf) { return float((hf.w >> HF_CONF_SHIFT) & 0xFFu) * (1.0 / 255.0); }
uvec4 HFSetConfidence(uvec4 hf, float c) {
    uint q = uint(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    return uvec4(hf.x, hf.y, hf.z,
                 (hf.w & 0x00FFFFFFu) | (max(q, 1u) << HF_CONF_SHIFT));
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
