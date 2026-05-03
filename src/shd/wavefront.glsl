// =============================================================================
// wavefront.glsl — shared layout for the decoupled-shading path tracer.
// =============================================================================
// Included by generate_primary / schedule / emit / trace / shade / resolve.
// Every shader that needs the ring or shading list includes this so the
// binding/layout contract can't drift between stages.
//
// Pipeline summary (see renderer.cpp for dispatch order):
//   generate_primary.comp   full-res      pixels -> GBuffer
//   schedule.comp           full-res      GBuffer voxels -> ShadeList (deduped)
//   emit.comp               1D sparse     ShadeList -> Ring (primary bounce)
//   trace.comp              1D ring       Ring rays -> Ring hits
//   shade.comp              1D ring       Ring hits -> lBuffer writes + survivors
//   resolve.comp            full-res      GBuffer voxelID -> lBuffer -> output
// =============================================================================

// Capacity for the in-flight ray ring. 48 B per payload * 262144 = 12 MiB.
// Sized so that SHADE_BUDGET typically fits in one chunk without wrapping. 262144
#define RAY_CAPACITY 49152u

// Bounce depth cap. Rays whose bounce_idx reaches this value are treated as
// terminated by shade.comp (they're skipped without writing a survivor).
#define MAX_BOUNCES 8u

// Russian-roulette activates after this many bounces. Before then every ray
// survives; after, survival probability = clamp(max(throughput), 0.05, 0.95).
#define RR_START_BOUNCE 2u

// Hit payload packed into the same Ray slot between trace and shade.
// trace.comp writes (hit, voxel_id, hit_pos_pckd, material) fields; shade.comp
// reads them and then overwrites the slot with either a survivor ray or marks
// it dead by setting bounce_idx >= MAX_BOUNCES.
//
// Why we carry `direct_light` and `indirect_light` separately in the payload:
// a path sample's final value is Σ emissive_k × Π_{j<k} throughput_j plus
// the skybox-on-miss term. Direct (NEE + bounce-0 emissive hit) converges
// fast — variance dies in 2-3 frames with RIS; indirect (bounce>=1
// contributions, deeper-bounce skybox) needs 50+ frames. Storing them in
// the same EMA slot couples their blend rates, polluting the variance signal
// rank.comp uses to allocate budget. Splitting the channels lets the lBuffer
// run two EMAs at different SAMPLE_CAPs (direct fast, indirect slow) — see
// shade.comp::depositSampleDual.
struct Ray {
    vec3  origin;              //  0..11
    float _pad0;               // 12..15  (std430 alignment — vec3 padded to 16)
    vec3  direction;           // 16..27
    float _pad1;               // 28..31
    vec3  throughput;          // 32..43  accumulated rayColor along the chain
    uint  primary_voxel_id;    // 44..47  lBuffer slot that every sample deposits into
    vec3  direct_light;        // 48..59  NEE + bounce-0 emissive hit
    uint  rng_state;           // 60..63  per-ray RNG
    vec3  indirect_light;      // 64..75  bounce>=1 emissive + skybox-on-miss
    uint  bounce_idx;          // 76..79  >= MAX_BOUNCES = terminated
    uint  hit_voxel_id;        // 80..83  filled by trace; 0 on miss
    uint  hit_material;        // 84..87  filled by trace
    uint  _pad2;               // 88..91
    uint  _pad3;               // 92..95  std430 stride padding
};  // sizeof == 96 B (RAY_STRIDE in renderer.cpp must match).

// --- Ring buffer (producer = emit / shade survivors, consumer = trace / shade) -----
layout(std430, binding = 0) buffer RayRing { Ray rays[]; };
layout(std430, binding = 1) buffer RingMeta {
    uint ring_head;    // producer cursor (mod RAY_CAPACITY)
    uint ring_tail;    // consumer cursor (mod RAY_CAPACITY)
    uint _ring_pad0;
    uint _ring_pad1;
};

// --- Sparse shading list: list of unique visible voxels this frame. ---------
// Entry layout: (voxelID, representative_pixel_index). representative_pixel is
// the linear pixel index (y*W + x) of the pixel that won the atomic claim for
// this voxel. emit.comp reads the GBuffer at that pixel to reconstruct origin
// and direction for the primary ray.
layout(std430, binding = 2) buffer ShadeList { uvec2 shade_list[]; };
layout(std430, binding = 3) buffer ShadeListMeta {
    uint shade_count;
    uint _sl_pad0;
    uint _sl_pad1;
    uint _sl_pad2;
};

// --- Emissive voxel registry (NEE) ------------------------------------------
// Built incrementally at runtime: shade.comp registers each emissive voxel it
// encounters via an atomic claim against the per-pass `emissiveClaim` image,
// then appends a packed entry here. Persistent across frames — once a light
// is found, it stays in the list until the renderer explicitly clears it
// (e.g. on scene rebuild).
//
// std430 layout: 32 B per entry (vec3 emission + uint vid + uvec3 pos + uint
// size). Each field is naturally 16-byte aligned so the array stride is exact.
struct EmissiveVoxel {
    vec3  emission;     // mat.color.rgb * mat.emissiveIntensity (scene units)
    uint  voxel_id;     // voxelKey(voxel) + 1; 0 reserved for "empty"
    uvec3 pos;          // voxel lattice min corner
    uint  size;         // voxel lattice extent (1, 2, 4, …)
};

layout(std430, binding = 4) buffer EmissiveList { EmissiveVoxel emissives[]; };
layout(std430, binding = 5) buffer EmissiveMeta {
    uint emissive_count;
    uint _em_pad0;
    uint _em_pad1;
    uint _em_pad2;
};

// Cap on the in-list emissive entries. 4096 lights × 32 B = 128 KB. The claim
// image's width × probes must comfortably exceed this so dedup hashing stays
// sparse. Tune up if scenes become emissive-heavy.
#define EMISSIVE_CAPACITY     4096u
#define EMISSIVE_CLAIM_W      8192u
#define EMISSIVE_CLAIM_PROBES 8

// --- Candidate list (importance-aware scheduling) ---------------------------
// schedule.comp emits one Candidate per UNIQUE visible voxel (deduped via
// claimMap). rank.comp consumes the list, scores each by pixel coverage +
// lBuffer variance, and writes the final shade_list with multiplicity (a
// high-importance voxel appears 2× so it gets two samples; a mid voxel 1×; a
// low voxel is dropped). This is the importance-aware rebuild of the old
// `taken < samplesPerVoxel` quota — same per-voxel sample-count knob, but
// allocated by *which* voxels need it most rather than first-come-first-served.
//
// claim_h / claim_p give rank.comp the (row, probe) into sampleCountMap so it
// can read the final pixel-coverage count without re-probing the claim hash.
struct Candidate {
    uint vid;
    uint pix_index;
    uint claim_h;
    uint claim_p;
};

layout(std430, binding = 6) buffer CandidateList { Candidate candidates[]; };
layout(std430, binding = 7) buffer CandidateMeta {
    uint candidate_count;
    uint _cm_pad0;
    uint _cm_pad1;
    uint _cm_pad2;
};

// Importance-distribution statistics + persistent thresholds.
//
// LAYOUT IN TWO HALVES:
//   * Persistent (offsets 0..15): thigh_bits / tmid_bits — written by
//     threshold.comp at the END of frame N, read by rank.comp at the START
//     of frame N+1. NOT zeroed in the per-frame reset.
//   * Per-frame (offsets 16..1055): min/max/count + 256-bin histogram, all
//     reset to 0 (or sentinels for min) at the start of each frame.
//
// Why GPU-side thresholds: the previous design read min/max back to the CPU
// at end-of-frame to derive thigh/tmid for the next frame. That readback is
// a sync point — it stalls the pipeline until the GPU drains. With a 256-bin
// histogram + a tiny scan kernel (threshold.comp), the same percentile-based
// thresholds are derived entirely on-GPU and stay in this SSBO. min/max bits
// are still computed for debugging / display, but no longer drive scheduling.
//
// Bin count of 256 keeps threshold.comp single-threaded scan trivially fast
// while giving 1/256 ≈ 0.4% precision on the percentile cutoffs — well below
// the noise floor of an importance-tier classifier.
layout(std430, binding = 8) buffer RankStats {
    // --- persistent ---
    uint thigh_bits;     // floatBitsToUint(tHigh) — used by rank.comp
    uint tmid_bits;      // floatBitsToUint(tMid)
    uint _rs_pad0;
    uint _rs_pad1;
    // --- reset every frame ---
    uint min_imp_bits;   // floatBitsToUint(min importance this frame)
    uint max_imp_bits;   // floatBitsToUint(max importance this frame)
    uint stats_count;    // == candidate_count
    uint _rs_pad2;
    uint histogram[256]; // imp ∈ [0,1] bucketed into 256 bins
};

// Cap on candidate entries. 1M × 16 B = 16 MB. Bounded by unique visible
// voxels per frame, which is generally << total pixels even at 1080p.
#define CANDIDATE_CAPACITY 1048576u
#define HISTOGRAM_BINS     256u

// rBuffer (ReSTIR DI per-voxel reservoir store, image bound in shade.comp at
// unit 3, R32UI). Same hash family as lBuffer (vid → row), but a separate
// image so the EMA accumulator and the reservoir state don't fight each other.
// rBufferStride uints per slot:
//   [0] vid (claim, 0=empty)        [4] Wsum_bits (sum of RIS weights, float)
//   [1] light_idx (chosen)          [5] M (sample count, capped at restirMaxM)
//   [2] light_vid (validation key)  [6] target_pdf_bits (phat at chosen sample)
//   [3] face_seed (face|uv16|uv16)  [7] last_time (frame counter, stale-reset)
// Hash: (vid % (rBufferWidth-1)) + 1, identical to lBuffer's. The slot count
// is intentionally smaller than lBuffer's (16 vs 32) — reservoirs change once
// per frame per voxel, and the M-cap means a stale slot self-decays anyway,
// so we don't need lBuffer's amount of probing depth.
#define rBufferStride 8

// lBuffer slot stride. 12 uints per slot = 48 B. Layout per slot:
//   [0]  vid
//   [1]  dir_count           — count of samples deposited into direct EMA
//   [2..4] dir_r/g/b sums    — Σ urgb (per-sample, see depositSampleDual)
//   [5]  dir_L²              — Σ luma²
//   [6]  ind_count           — count of samples deposited into indirect EMA
//   [7..9] ind_r/g/b sums
//   [10] ind_L²
//   [11] last_time           — frame counter for stale-reset / LRU eviction
// Direct and indirect maintain independent counts so each can use a different
// SAMPLE_CAP (halve-at-cap is the EMA blend mechanism here): direct caps low
// for fast response (NEE converges in ~3 frames), indirect caps high for
// smooth low-variance averaging (GI needs many bounces to settle).
#define lBufferStride 12

// Pack a 3D voxel position into 30 bits (10 per axis). Matches voxelKey()
// packing in internal.glsl and survives octree relinearisation.
uint packVoxelPos(uvec3 p) {
    return (p.x & 0x3FFu) | ((p.y & 0x3FFu) << 10u) | ((p.z & 0x3FFu) << 20u);
}
uvec3 unpackVoxelPos(uint p) {
    return uvec3(p & 0x3FFu, (p >> 10u) & 0x3FFu, (p >> 20u) & 0x3FFu);
}
