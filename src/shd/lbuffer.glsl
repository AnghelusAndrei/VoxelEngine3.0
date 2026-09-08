#include "constants.glsl"


layout(std430, binding = SSBO_LBUFFER_BINDING) coherent buffer LBuffer { uint data[]; } lbuf;

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
        if (lbuf.data[sb + LB_TIMESTAMP] == 0u) return LB_NO_SLOT;             // pristine empty => absent
    }
    return LB_NO_SLOT;
}

// O(1) lookup — the per-pixel path. `nodeLo` is the voxel's octree node Lo word (the caller
// indexes it directly with the gbuffer's `vid`; no traversal). claim baked this voxel's probe
// distance into its free bits, so the slot is just home+offset.
//
// The full key is verified on the slot's OWN cache line — D0/D1 sit in the same 64 B as the
// channels the caller reads next, so the check is free. That verify is what makes every
// wrong-offset case safe and self-describing, with no validity bit and no sentinel:
//   * never written / zeroed by a CPU node write -> offset 0 -> home slot -> key mismatch
//   * voxel was evicted, another key now owns the slot        -> key mismatch
//   * garbage bits from a recycled node                       -> masked in range, key mismatch
// All of them return LB_NO_SLOT, which is just the ordinary hole state the fill pass absorbs.
// Offset 0 is therefore unambiguous: it means "at home" exactly when the key agrees.
uint lookupLBuffer(uvec3 pos, uint level, uint version, uint nodeLo){
    uint off = (nodeLo >> NODE_SLOTOFF_SHIFT) & NODE_SLOTOFF_MASK;
    uint s   = (homeSlot(pos, level, version) + off) & (LBUFFER_SLOTS_TOTAL - 1u);
    uint sb  = s * SLOT_DWORDS;
    uvec2 key = packSlotKey(pos, level, version);
    return (lbuf.data[sb] == key.x && lbuf.data[sb + 1u] == key.y) ? s : LB_NO_SLOT;
}

// O(1) lookup with the probe as a correctness net. Use this wherever the caller has the
// voxel's node word in hand
uint findLBuffer(uvec3 pos, uint level, uint version, uint nodeLo){
    uint s = lookupLBuffer(pos, level, version, nodeLo);
    return (s != LB_NO_SLOT) ? s : probeLBuffer(pos, level, version);
}

// Is a slot's channel still trustworthy? `window == 0` disables the check.
bool fresh(uint age, uint window){ return window == 0u || age <= window; }

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
// Packs into bits 2..31 (flag bit 0 left clear => "normal valid").
uint PackNormal(vec3 n){
    return (pkNormC(n.x) << 22u) | (pkNormC(n.y) << 12u) | (pkNormC(n.z) << 2u);
}

// Rec. 709 luma weights, for LINEAR light - which is what the diffuse and specular
// channels are (irradiance E/pi and outgoing radiance, never gamma-encoded). The
// Rec. 601 weights are for gamma-encoded video and would be wrong here.
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

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