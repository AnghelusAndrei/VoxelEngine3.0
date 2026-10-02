#ifndef OCTREE_GLSL
#define OCTREE_GLSL

// Node decode and POINT LOOKUP. Deliberately separate from internal.glsl: that file declares
// `shared uvec2 gs_stack[MAXDEPTH][64]` for Raycast's descent, and shared memory is what caps
// occupancy in the heavy passes. filter.comp and normal.comp need to resolve a position to a
// leaf but never trace a ray, so they take this header and pay nothing.
//
// The including shader must have declared the octree SSBO as `octree`.
//
// 64-bit node, AoS in octree.nodes[] (uvec2 per slot: .x = lo, .y = hi).
//   Lo: [ isNode:1 | material:10 | childmask:8 | version:8 | slotOffset:5 ]  Hi: [ next:32 ]
//   Field shifts/masks live in constants.glsl (NODE_* / MATERIAL_*).
// Mirrors Octree::Node on the CPU — keep both sides on these explicit masks.

struct Node {
    bool type;
    uint childmask, next, material, version;
};

Node UnpackNode(uvec2 raw) {
    uint lo = raw.x;
    return Node(
        bool(lo & NODE_ISNODE_BIT),
        (lo >> NODE_CM_SHIFT)  & NODE_CM_MASK,    // childmask
        raw.y,                                    // next (full 32-bit hi word)
        (lo >> NODE_MAT_SHIFT) & MATERIAL_MASK,   // material
        (lo >> NODE_VER_SHIFT) & NODE_VER_MASK    // version
    );
}

uint locate(uvec3 pos, uint p2) {
    return (uint(bool(pos.x & p2)) << 2u) | (uint(bool(pos.y & p2)) << 1u) | uint(bool(pos.z & p2));
}

// Two semantics off one descent, because the two callers want different things:
//   .hit  — is this position SOLID? An internal node at maxLevel counts: something below it is
//           filled, which is what an occupancy gradient (normal.comp, shade's fallback) asks.
//   .leaf — is it a real leaf, so id/version/level address a cache slot? A cache tap
//           (filter.comp) needs a key to hash, and an internal node has none.
// Getting these two backwards silently changes every computed normal, so they are named rather
// than inferred from `material != 0`.
struct point_t {
    bool  hit;
    bool  leaf;
    uint  id, material, version, level;
    uvec3 position;   // leaf origin, p & ~(size-1)
};

// Descend to the leaf containing `p`, at most `maxLevel` deep. `Lng` is the octree edge length
// (1 << octreeDepth) — passed rather than read from a global so normal.comp, which never sets
// internal.glsl's `octreeLength`, can use this too.
//
// Empty octants are skipped on the PARENT's childmask, exactly as Raycast does, so an absent
// child costs no fetch at all.
point_t descendAt(uvec3 p, uint maxLevel, uint Lng) {
    point_t o = point_t(false, false, 0u, 0u, 0u, 0u, uvec3(0u));
    if (any(greaterThanEqual(p, uvec3(Lng)))) return o;

    Node n = UnpackNode(octree.nodes[0]);
    if (!n.type) {
        // Degenerate single-leaf root. Solid if it has a material, but NOT addressable as a
        // cache slot: a real leaf has level >= 1 (level 0 is the gbuffer's miss sentinel), so
        // this can never have been seated by claim.
        o.hit = (n.material != 0u);
        return o;
    }

    uint block = n.next;
    for (uint d = 1u; d <= maxLevel; d++) {
        uint p2  = Lng >> d;
        uint idx = locate(p, p2);
        if ((n.childmask & (1u << idx)) == 0u) return o;     // empty octant

        uint  slot = block + idx;
        n = UnpackNode(octree.nodes[slot]);

        if (!n.type) {                                        // leaf
            if (n.material == 0u) return o;                   // empty leaf
            return point_t(true, true, slot, n.material, n.version, d, p & ~uvec3(p2 - 1u));
        }
        if (d == maxLevel)                                    // internal node at the query level
            return point_t(true, false, slot, 0u, n.version, d, p & ~uvec3(p2 - 1u));
        block = n.next;
    }
    return o;
}

#endif // OCTREE_GLSL
