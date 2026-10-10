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

// O(1) lookup
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

// Lock-free claim of `key` from its home slot `h`. Returns the slot or
// LB_NO_SLOT. Shared by the voxel and ancestor claims, which differ only in what they do after:
//   inserted  — the slot was empty or evicted and now carries `key`; the caller inits the payload
//   prevStamp — on a MATCH, the timestamp the slot had before this frame (staleness)
// Requires one thread per key per frame (the dedup), and frameStamp >= 1.
uint lbClaim(uvec2 key, uint h, uint frameStamp, out bool inserted, out uint prevStamp){
    inserted  = false;
    prevStamp = 0u;
    int  best   = -1;
    uint bestTs = 0xFFFFFFFFu;

    for (uint i = 0u; i < LB_PROBE_LIMIT; i++){
        uint s  = (h + i) & (LBUFFER_SLOTS_TOTAL - 1u);
        uint sb = s * SLOT_DWORDS;
        uint d2 = lbuf.data[sb + LB_TIMESTAMP];

        if (lbuf.data[sb] == key.x && lbuf.data[sb + 1u] == key.y){          // MATCH
            if (atomicCompSwap(lbuf.data[sb + LB_TIMESTAMP], d2, frameStamp) == d2){
                prevStamp = d2;
                return s;
            }
            continue;                                                         // evicted under us
        }
        if (d2 == 0u){                                                        // EMPTY
            if (atomicCompSwap(lbuf.data[sb + LB_TIMESTAMP], 0u, frameStamp) == 0u){
                lbuf.data[sb]      = key.x;
                lbuf.data[sb + 1u] = key.y;
                inserted = true;
                return s;
            }
            continue;
        }
        if (d2 < frameStamp && d2 < bestTs){ bestTs = d2; best = int(s); }
    }

    if (best >= 0){                                                           // EVICT stalest in the window
        uint sb = uint(best) * SLOT_DWORDS;
        if (atomicCompSwap(lbuf.data[sb + LB_TIMESTAMP], bestTs, frameStamp) == bestTs){
            lbuf.data[sb]      = key.x;
            lbuf.data[sb + 1u] = key.y;
            inserted = true;
            return uint(best);
        }
        // lost the eviction race → NO_SLOT (rare, only near full load)
    }
    return LB_NO_SLOT;
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

// ---- ancestor slots (constants.glsl § ancestor slots) ----------------------------------
// Cube bin for a normal component on `axis`: +X -X +Y -Y +Z -Z.
uint ancBin(uint axis, float c) { return 2u * axis + (c < 0.0 ? 1u : 0u); }

float ancCount(uint sb, uint bin) {
    vec2 p = unpackHalf2x16(lbuf.data[sb + LB_ANC_COUNT + (bin >> 1u)]);
    return ((bin & 1u) == 0u) ? p.x : p.y;
}

// The cube at normal N, sample-weighted over N's three bins
vec3 ancEval(uint sb, vec3 N, out float avail, out float cover) {
    vec3 E = vec3(0.0); avail = 0.0; cover = 0.0;
    for (uint a = 0u; a < 3u; a++) {
        float p = N[a] * N[a];
        uint  b = ancBin(a, N[a]);
        float c = ancCount(sb, b);
        if (p <= 0.0 || c <= 0.0) continue;
        E += p * c * unpackRGB9E5(lbuf.data[sb + LB_ANC_BINS + b]);
        avail += p * c;
        cover += p;
    }
    cover /= max(dot(N, N), 1e-8);
    return (avail > 0.0) ? E / avail : vec3(0.0);
}

// Direction-blind: every bin, sample-weighted — what a scalar aggregate would have stored.
vec3 ancFlat(uint sb, out float avail) {
    vec3 E = vec3(0.0); avail = 0.0;
    for (uint b = 0u; b < 6u; b++) {
        float c = ancCount(sb, b);
        E += c * unpackRGB9E5(lbuf.data[sb + LB_ANC_BINS + b]);
        avail += c;
    }
    return (avail > 0.0) ? E / avail : vec3(0.0);
}

// One level of the sample-budget borrow, shared by compose, resolve's hole fill and shade's bounce
// fallback. A voxel short of `need` samples takes from each ancestor, nearest first, only what that
// level ADDS over the levels below it (its count includes theirs):
//   take = min(need - took, avail - below)
// The caller ends with (n·x + acc) / (n + took). Returns this level's take.
float ancBorrow(uint asb, vec3 N, bool hasN, float need,
                inout float took, inout float below, inout vec3 acc) {
    float avail, cover;
    vec3  E = hasN ? ancEval(asb, N, avail, cover) : ancFlat(asb, avail);
    float t = min(need - took, max(avail - below, 0.0));
    acc   += t * E;
    took  += t;
    below  = max(below, avail);
    return t;
}
