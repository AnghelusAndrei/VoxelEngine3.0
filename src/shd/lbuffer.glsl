// Shared lBuffer addressing, voxel key, probe, and HDR helpers. Mirrors Renderer's
// LBUFFER_* constants. Including shaders get the slot data at SSBO binding 2. The cache
// is a FLAT, lock-free, OPEN-ADDRESSED table: slot = hash(key) & (N-1), linear probe.
// Claiming is by CAS on the timestamp word (see lbuffer_claim.comp / architecture/CLAIM.md)
// — no bucket lock, no retry buffers. A "slot" is a flat index 0..N-1.
//
// 16-DWORD slot (64 B, Milestone B):
//   0 : pos.x[0:15] | pos.y[16:31]                          \ key
//   1 : pos.z[0:15] | sizeLevel[16:23] | version[24:31]     /
//   2 : timestamp (monotonic frameStamp; 0 ⟺ pristine-empty / claim token)
//   3 : octNormal[2:31] | flag[0] | spare[1]
//   4 : diffuse  channel — irradiance, RGB9E5 (EMA result, read by resolve)
//   5 : specular channel — RGB9E5 (EMA result, read by resolve)
//   6 : N_diff[0:15] | N_spec[16:31]   (EMA sample counts)
//   7..9  : diffuse  accumulator R,G,B (u32, HDR fixed-point, atomicAdd in accum)
//   10    : pixelCount                 (u32, atomicAdd in accum)
//   11..13: specular accumulator R,G,B (u32, HDR fixed-point, atomicAdd in accum)
//   14 : spare (prevTimestamp — dynamic-lighting increment)
//   15 : spare (ReSTIR reservoir)

// Flat open-addressed table. N must be a power of two (so home = hash & (N-1)).
// Size N ≈ 2× peak visible voxels (keep load factor ≤ 0.5 → ~1.5 avg probes).
#define LBUFFER_SLOTS_TOTAL 4194304u   // 2^22 slots × 64 B = 256 MB
#define SLOT_DWORDS         16u
#define LB_PROBE_LIMIT      32u        // max linear probe length
#define LB_NO_SLOT          0xFFFFFFFFu

// Slot field offsets (DWORD index within a slot).
#define LB_KEY0      0u
#define LB_KEY1      1u
#define LB_TIMESTAMP 2u
#define LB_NORMAL    3u
#define LB_DIFFUSE   4u
#define LB_SPECULAR  5u
#define LB_SAMPLES   6u
#define LB_DIFF_ACC  7u    // R,G,B = 7,8,9
#define LB_PIXELS    10u
#define LB_SPEC_ACC  11u   // R,G,B = 11,12,13
#define LB_RESV_LIGHT 14u  // ReSTIR DI reservoir: chosen light-tree leafIdx<<8 | M (0 ⟹ no reservoir)
#define LB_RESV_W     15u  // ReSTIR DI reservoir: W (fp32 unbiased contribution weight)

// HDR fixed-point scale for the u32 atomic accumulators. Per-sample radiance is
// firefly-clamped (FIREFLY_CLAMP); max accumulated value per channel per frame is
// FIREFLY_CLAMP * ACCUM_SCALE * pixelsPerVoxel. At the default virtualScale (≈130K
// virtual pixels) a screen-filling voxel stays < 2^32; very low virtualScale + a
// screen-filling clamped voxel can saturate (rare; radiance is ~[0,2] in practice).
#define ACCUM_SCALE   1024.0
#define FIREFLY_CLAMP 16.0

layout(std430, binding = 2) coherent buffer LBuffer { uint data[]; } lbuf;

uint lb_hash(uint x){ x ^= x>>16; x *= 0x7feb352du; x ^= x>>15; x *= 0x846ca68bu; x ^= x>>16; return x; }

uint voxelKey(uvec3 pos, uint level, uint version){
    uint h = lb_hash(pos.x);
    h = lb_hash(h ^ pos.y);
    h = lb_hash(h ^ pos.z);
    h = lb_hash(h ^ level);
    h = lb_hash(h ^ version);
    return h & 0x7FFFFFFFu;          // non-zero (0 reserved)
}

// Home slot for a key (open addressing); linear-probe forward from here.
uint homeSlot(uvec3 pos, uint level, uint version){
    return voxelKey(pos, level, version) & (LBUFFER_SLOTS_TOTAL - 1u);
}
uint slotBase(uint slot){ return slot * SLOT_DWORDS; }
uint linearSlotToBase(uint slot){ return slot * SLOT_DWORDS; }   // slot index is already flat

// Slot identity (DWORD0,1) for a voxel key.
uvec2 packSlotKey(uvec3 pos, uint level, uint version){
    return uvec2(
        (pos.x & 0xFFFFu) | (pos.y << 16u),
        (pos.z & 0xFFFFu) | ((level & 0xFFu) << 16u) | ((version & 0xFFu) << 24u)
    );
}

// Read-only linear probe: flat slot for (pos,level,version), or LB_NO_SLOT.
// Stops at the first pristine-empty (D2==0): a key always sits before the first
// empty in its run (insert claims the first empty; eviction overwrites, never deletes).
uint probeLBuffer(uvec3 pos, uint level, uint version){
    uvec2 key = packSlotKey(pos, level, version);
    uint  h   = homeSlot(pos, level, version);
    for (uint i = 0u; i < LB_PROBE_LIMIT; i++){
        uint s  = (h + i) & (LBUFFER_SLOTS_TOTAL - 1u);
        uint sb = s * SLOT_DWORDS;
        if (lbuf.data[sb] == key.x && lbuf.data[sb + 1u] == key.y) return s;   // match
        if (lbuf.data[sb + LB_TIMESTAMP] == 0u) return LB_NO_SLOT;             // pristine empty → absent
    }
    return LB_NO_SLOT;
}

// Slot DWORD3 layout:  flag[0] | spare[1] | z[2:11] | y[12:21] | x[22:31]
// 3×10-bit signed unit vector. Flag at bit 0 so it never collides with the normal.
#define NORMAL_UPDATE_BIT 1u

vec3 UnpackNormal(uint p) {
    return vec3(
        float((p >> 22u) & 0x3FFu) / 511.0 - 1.0,
        float((p >> 12u) & 0x3FFu) / 511.0 - 1.0,
        float((p >>  2u) & 0x3FFu) / 511.0 - 1.0
    );
}

uint pkNormC(float c){ return uint(clamp((c + 1.0) * 511.0, 0.0, 1023.0)) & 0x3FFu; }
// Packs into bits 2..31 (flag bit 0 left clear → "normal valid").
uint PackNormal(vec3 n){
    return (pkNormC(n.x) << 22u) | (pkNormC(n.y) << 12u) | (pkNormC(n.z) << 2u);
}

// ---- RGB9E5 shared-exponent HDR packing (used by avg writer / resolve reader) ----
uint packRGB9E5(vec3 rgb){
    const float MAXVAL = (511.0 / 512.0) * exp2(31.0 - 15.0);   // 65408.0
    rgb = clamp(rgb, vec3(0.0), vec3(MAXVAL));
    float maxc = max(max(rgb.r, rgb.g), rgb.b);
    float expS = max(-16.0, floor(log2(max(maxc, 1e-30)))) + 1.0 + 15.0;  // biased exponent
    float denom = exp2(expS - 15.0 - 9.0);
    if (floor(maxc / denom + 0.5) >= 512.0) { denom *= 2.0; expS += 1.0; } // rounding overflow
    uvec3 m = uvec3(floor(rgb / denom + 0.5));
    return (m.r & 0x1FFu) | ((m.g & 0x1FFu) << 9u) | ((m.b & 0x1FFu) << 18u) | (uint(expS) << 27u);
}
vec3 unpackRGB9E5(uint v){
    float scale = exp2(float(v >> 27u) - 15.0 - 9.0);
    return vec3(float(v & 0x1FFu), float((v >> 9u) & 0x1FFu), float((v >> 18u) & 0x1FFu)) * scale;
}

// ---- ReSTIR DI reservoir packing (LB_RESV_LIGHT=D14, LB_RESV_W=D15). architecture/RESTIR.md ----
// D14 = lightPos.x[0:8] | lightPos.y[9:17] | lightPos.z[18:26] | M[27:31].  M==0 ⟹ NO reservoir.
// Assumes octree depth ≤ 9 (each pos axis < 512 = 9 bits). renderer.cpp warns if depth > 9.
// D15 = W (fp32 unbiased contribution weight).
uint  packResvLight(uvec3 p, uint M){
    return (p.x & 0x1FFu) | ((p.y & 0x1FFu) << 9u) | ((p.z & 0x1FFu) << 18u) | ((M & 0x1Fu) << 27u);
}
uvec3 unpackResvPos(uint d){ return uvec3(d & 0x1FFu, (d >> 9u) & 0x1FFu, (d >> 18u) & 0x1FFu); }
uint  unpackResvM  (uint d){ return (d >> 27u) & 0x1Fu; }

// ReSTIR DI spatiotemporal reservoir — a PARALLEL buffer (binding 6, 2 DWORDs/slot), separate from the
// 64 B slot. Per-frame loop: temporal (restir.comp) → slot D14/D15, spatial (restir_spatial.comp) reads
// the slot snapshot + writes HERE, shade reads HERE, next-frame temporal reads HERE as prev → the loop
// COMPOUNDS (the real ReSTIR accelerator). Race-free (spatial reads slot, writes this — disjoint). MUST
// be zeroed in initSlot on claim/evict (else a reused slot serves a prior voxel's reservoir → wrong light
// on disocclusion). [0]=pos|M (packResvLight), [1]=W (fp32). architecture/RESTIR.md §E1.
#define RESV_SP_DWORDS 2u
layout(std430, binding = 6) buffer ResvSpatial { uint data[]; } resvSp;
uint resvSpBase(uint slot){ return slot * RESV_SP_DWORDS; }
