// Shared gbuffer pixel format (RGBA32UI, full-res). Single source of truth for
// primary.comp (writer) and resolve.comp / later per-voxel passes (readers).
//
//   x : pos.x[0:15] | pos.y[16:31]
//   y : pos.z[0:15] | sizeLevel[16:23] | version[24:31]
//   z : material[0:6]
//   w : DDA step count (debug STEPS heatmap; written on hit AND miss)
//
// `sizeLevel` is the octree depth of the hit (root = 0, finest = octreeDepth), so
// every real hit has sizeLevel >= 1. A miss writes sizeLevel == 0 (w may carry steps).

struct GBuffer { bool hit; uvec3 pos; uint level; uint material; uint version; uint steps; };

uvec4 PackGBuffer(uvec3 pos, uint level, uint material, uint version, uint steps) {
    return uvec4(
        (pos.x & 0xFFFFu) | (pos.y << 16u),
        (pos.z & 0xFFFFu) | ((level & 0xFFu) << 16u) | ((version & 0xFFu) << 24u),
        material & 0x7Fu,
        steps
    );
}

GBuffer UnpackGBuffer(uvec4 g) {
    GBuffer o;
    o.level    = (g.y >> 16u) & 0xFFu;
    o.hit      = (o.level != 0u);
    o.pos      = uvec3(g.x & 0xFFFFu, g.x >> 16u, g.y & 0xFFFFu);
    o.version  = (g.y >> 24u) & 0xFFu;
    o.material = g.z & 0x7Fu;
    o.steps    = g.w;
    return o;
}
