// Emissive-voxel light SVO — sampled by descent (architecture/LIGHTTREE.md).
// Flat node array at SSBO binding 5; 3 uints / node (12 B), mirroring the octree's lo/hi:
//   [0] power (float bits)
//   [1] childBase  (node-index of the 8-node child block; 0 ⟹ leaf)   — like octree `next`
//   [2] isNode:1 | material:7 | childMask:8 | reserved:16             — like octree `lo`
// Position/level are NOT stored — reconstructed by accumulating octant offsets during descent.
// The including shader must declare `uniform uint octreeDepth;`.

layout(std430, binding = 5) readonly buffer LightTreeBuffer { uint nodes[]; } ltree;

struct LightSample {
    bool  valid;     // false ⟹ no emissive voxels in the scene
    vec3  center;    // sampled emissive voxel center (world units)
    uvec3 pos;       // its voxel min-corner (for the exact occlusion test: hit.position == pos)
    uint  size;      // its voxel size (emitting area = size²)
    uint  material;  // its material id (→ color · emissiveIntensity)
    float pdfDisc;   // discrete probability of selecting this leaf = Π(childPower/total)
};

uint  lt_word2    (uint node) { return ltree.nodes[node * 3u + 2u]; }
uint  lt_childBase(uint node) { return ltree.nodes[node * 3u + 1u]; }
float lt_power    (uint node) { return uintBitsToFloat(ltree.nodes[node * 3u]); }

// Self-contained PCG (lt_ prefix to avoid clashing with an includer's own rng).
uint  lt_rand(inout uint s){ s = s*747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
float lt_rnd (inout uint s){ return float(lt_rand(s)) * (1.0 / 4294967296.0); }

// Descend the SVO, picking each child ∝ its subtree power, accumulating the leaf's origin as
// we go. Returns the selected leaf and the discrete selection pdf (Π branch probabilities).
LightSample sampleLightTree(inout uint rng){
    LightSample ls;
    ls.valid = false; ls.material = 0u; ls.center = vec3(0.0); ls.pdfDisc = 1.0;
    if (lt_power(0u) <= 0.0) return ls;                      // no lights

    uint  node   = 0u;
    uvec3 origin = uvec3(0u);
    uint  size   = 1u << octreeDepth;                        // root covers the whole volume
    for (int guard = 0; guard < 24; guard++){
        uint w2 = lt_word2(node);
        if ((w2 & 1u) == 0u){                               // LEAF (isNode bit clear)
            ls.material = (w2 >> 1u) & 0x7Fu;
            ls.pos      = origin;
            ls.size     = size;
            ls.center   = vec3(origin) + float(size) * 0.5;
            ls.valid    = true;
            return ls;
        }
        // INTERNAL: linear CDF over present children (childMask), tracking the chosen octant.
        uint  childBase = lt_childBase(node);
        uint  mask      = (w2 >> 8u) & 0xFFu;
        float total     = lt_power(node);
        float r         = lt_rnd(rng) * total;
        float acc       = 0.0;
        uint  chosen    = childBase;
        uint  chosenOct = 0u;
        float cp        = total;
        for (uint o = 0u; o < 8u; o++){
            if ((mask & (1u << o)) == 0u) continue;
            float pw = lt_power(childBase + o);
            chosen = childBase + o; chosenOct = o; cp = pw;  // fallback = last present child (fp guard)
            acc += pw;
            if (r < acc) break;
        }
        ls.pdfDisc *= cp / total;
        uint h = size >> 1u;
        origin += uvec3((chosenOct & 4u) != 0u ? h : 0u,
                        (chosenOct & 2u) != 0u ? h : 0u,
                        (chosenOct & 1u) != 0u ? h : 0u);
        size = h;
        node = chosen;
    }
    return ls;   // guard tripped — should not happen for a well-formed tree
}

// Outward normal of the voxel face that faces direction `d` (voxel centre → shading point).
// Shared by the NEE / MIS geometry on both shade.comp and restir.comp.
vec3 dominantFaceNormal(vec3 d){
    vec3 ad = abs(d);
    return (ad.x >= ad.y && ad.x >= ad.z) ? vec3(d.x < 0.0 ? -1.0 : 1.0, 0.0, 0.0)
         : (ad.y >= ad.z)                 ? vec3(0.0, d.y < 0.0 ? -1.0 : 1.0, 0.0)
                                          : vec3(0.0, 0.0, d.z < 0.0 ? -1.0 : 1.0);
}

// Reverse of sampleLightTree: descend to the emissive leaf containing `leafPos`, returning its
// material (0 ⟹ absent / not sampleable) and, via `pdfDisc`, the probability the descent WOULD
// select it (Π childPower/total == that leaf's forward pdfDisc). Used by MIS (BSDF-hit weight) and
// ReSTIR (re-evaluate a stored sample's target). architecture/RESTIR.md §MIS rule.
uint lightLeafLookup(uvec3 leafPos, out float pdfDisc){
    pdfDisc = 0.0;
    if (lt_power(0u) <= 0.0) return 0u;
    uint  node = 0u;
    uint  size = 1u << octreeDepth;
    float pdf  = 1.0;
    for (int guard = 0; guard < 24; guard++){
        uint w2 = lt_word2(node);
        if ((w2 & 1u) == 0u){ pdfDisc = pdf; return (w2 >> 1u) & 0x7Fu; }   // reached the leaf
        uint h   = size >> 1u;
        uint oct = ((leafPos.x & h) != 0u ? 4u : 0u)
                 | ((leafPos.y & h) != 0u ? 2u : 0u)
                 | ((leafPos.z & h) != 0u ? 1u : 0u);
        if ((((w2 >> 8u) & 0xFFu) & (1u << oct)) == 0u) return 0u;          // path absent
        uint  cb    = lt_childBase(node);
        float total = lt_power(node);
        if (total <= 0.0) return 0u;
        pdf  *= lt_power(cb + oct) / total;
        node  = cb + oct;
        size  = h;
    }
    return 0u;   // guard tripped (malformed tree)
}

// pdf-only convenience (the MIS BSDF-hit weight just needs the selection probability).
float lightTreePdf(uvec3 leafPos){ float p; lightLeafLookup(leafPos, p); return p; }
