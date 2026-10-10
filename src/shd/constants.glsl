#ifndef CONSTANTS_GLSL
#define CONSTANTS_GLSL

// ---- SSBO binding points --------------------------------------------------------------
#define SSBO_OCTREE_BINDING         0
#define SSBO_CLAIM_BINDING          1
#define SSBO_LBUFFER_BINDING        2
#define SSBO_UNIQUE_LIST_BINDING    3
#define SSBO_UNIQUE_COUNT_BINDING   4
#define SSBO_INDIRECT_ARGS_BINDING  5
#define SSBO_ACCUM_DIFFUSE_BINDING  6
#define SSBO_ACCUM_SPECULAR_BINDING 7
#define SSBO_MATERIAL_BINDING       8
#define SSBO_UNIQUE_ANCESTOR_LIST_BINDING   9
#define SSBO_UNIQUE_ANCESTOR_STARTS_BINDING 10
#define SSBO_UNIQUE_ANCESTOR_COUNTS_BINDING 11

// ---- lBuffer sizing (see LBUFFER.md) --------------------------------------------------
#define LBUFFER_SLOTS_TOTAL 4194304u   // 2^22 slots × 64 B = 256 MB
#define SLOT_DWORDS         16u
#define LB_PROBE_LIMIT      32u        // max linear probe length
#define LB_NO_SLOT          0xFFFFFFFFu
// Ancestor (internal-node) entries set this bit in the key's level byte. Depth never exceeds 15,
// so it cannot collide with a real level, and it keeps an ancestor apart from a maxSteps-fallback
// hit, which caches that same internal node as an ordinary voxel under (pos, level, version).
#define LB_ANCESTOR_FLAG    0x80u

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
//   7 : uniqueVoxelList index this frame
//   8 : EMA mean of log(luminance)      - float32 BIT PATTERN (floatBitsToUint)
//   9 : EMA variance of log(luminance)  - float32 BIT PATTERN (floatBitsToUint)
//  10 : EMA drift of log(luminance)     - float32 BIT PATTERN (floatBitsToUint)
//       MUST be read with uintBitsToFloat and written with floatBitsToUint.
//  11 : composed diffuse  channel — RGB9E5 (compose.comp; read by resolve and shade's bounce)
//  12 : published specular — RGB9E5, = D5 (compose.comp; ancestors are diffuse-only)
//  13 : compose info — packHalf2x16(alpha, deepest ancestor level borrowed from); 0 = own value
//   14->15 : spare (unused, reserved for future expansion)

// Slot field offsets (DWORD index within a slot).
#define LB_KEY0      0u
#define LB_KEY1      1u
#define LB_TIMESTAMP 2u
#define LB_NORMAL    3u
#define LB_DIFFUSE   4u
#define LB_SPECULAR  5u
#define LB_SAMPLES   6u
#define LB_LISTID    7u
#define LB_LUMINANCE 8u
#define LB_VARIANCE  9u
#define LB_DRIFT     10u
#define LB_FDIFFUSE  11u
#define LB_FSPECULAR 12u
#define LB_COMPOSE   13u

// ---- ancestor slots: octree internal nodes (ancestor_aggregate.comp) -----------------------
// Same table, same D0-D2 (key via PackAncestorKey, timestamp). Payload is a 6-bin ambient cube
// of the children's diffuse irradiance, so the two faces of a thin wall land in opposite bins,
// each bin with the number of samples behind it — all the sample-budget compose needs.
//   3      : octree node id of this ancestor (lbuffer_claimAnc, every frame it is claimed) —
//            compose walks from it to neighbouring cells by childmask, never probing a missing key
//   4..9   : per-bin sample-weighted mean irradiance, RGB9E5, bins +X -X +Y -Y +Z -Z
//   10..12 : per-bin sample count, packHalf2x16 pairs (0,1)(2,3)(4,5); 0 = no data.
//            Saturates at ANC_COUNT_MAX (fp16 max) — far above any useful budget.
//   13..15 : free
#define LB_ANC_NODE    3u
#define LB_ANC_BINS    4u
#define LB_ANC_COUNT   10u
#define ANC_COUNT_MAX  65504.0
// indirectArgs layout: [0] per voxel, [ANC_ARGS_BASE + d] ancestor claim at depth d (1 lane each),
// [ANC_ARGS_BASE + octreeDepth + d] aggregation at depth d (ANC_AGG_LANES lanes each).
#define ANC_ARGS_BASE  2u
#define ANC_AGG_LANES  8u


// ---- octree node Lo word ---------------------------------------------------------------
//   [ isNode:1 | material:10 | childmask:8 | version:8 | slotOffset:5 ]
#define MATERIAL_BITS        10u
#define MATERIAL_MASK        0x3FFu
#define MATERIAL_MAX         1024u

#define NODE_ISNODE_BIT      1u
#define NODE_MAT_SHIFT       1u
#define NODE_CM_SHIFT        11u
#define NODE_CM_MASK         0xFFu
#define NODE_VER_SHIFT       19u
#define NODE_VER_MASK        0xFFu
#define NODE_SLOTOFF_SHIFT   27u
#define NODE_SLOTOFF_MASK    0x1Fu

//accum.comp pixel irradiance scaling
#define ACCUM_SCALE   1024.0
#define FIREFLY_CLAMP 16.0
#define LUM_FLOOR      1e-2
#define VAR_ALPHA_MAX  0.2
#define VAR_ALPHA_MIN  0.03      // ~33-frame ceiling on variance memory, regardless of n

#define VAR_WINDOW_EPS 0.01 // Variance-adaptive EMA window: window = nDiffMax / (v + VAR_WINDOW_EPS).
#define VAR_WINDOW_MIN 3u        // never smooth over fewer than this
#define LB_COUNT_MAX   65535u // LB_SAMPLES packs two 16-bit counts, so a window past this wraps silently.
#define VAR_DISPLAY_MAX 1.5 // Full-scale point of the VARIANCE debug ramp, in log-luminance standard deviations.
#define IRR_HOLE 0xFFFFFFFFu   // stale or uncached => resolve's fillHole: own ancestors (diffuse), virtual texel (specular)

// ---- render modes: must mirror core::RenderType (guarded by static_assert in core.hpp) ----
#define MODE_OCTREE     0u
#define MODE_MATERIAL   1u
#define MODE_NORMAL     2u
#define MODE_VERSION    3u
#define MODE_CLAIM_AGE  4u
#define MODE_LRU        5u
#define MODE_VIRTUAL    6u
#define MODE_SHADE      7u
#define MODE_SHADING    8u
#define MODE_SAMPLES    9u
#define MODE_HOLES      10u
#define MODE_LUMINANCE  11u
#define MODE_VARIANCE   12u
#define MODE_COMPOSE    13u
#define MODE_UNFILTERED 14u
#define MODE_DIFFUSE    15u
#define MODE_SPECULAR   16u
#define MODE_ANCESTOR   17u

// MODE_ANCESTOR sub-views (`ancestorView`)
#define ANC_VIEW_CUBE    0u   // irradiance, cube evaluated at the voxel's normal
#define ANC_VIEW_FLAT    1u   // irradiance, all bins sample-weighted (what a scalar would leak)
#define ANC_VIEW_SAMPLES 2u   // samples behind the cube at the voxel's normal, log2 heat
#define ANC_VIEW_ALPHA   3u   // compose's alpha (D13): black = own value, white = all ancestor
#define ANC_VIEW_K       4u   // compose's deepest borrowed level (D13), heat over 0..4

#endif // CONSTANTS_GLSL