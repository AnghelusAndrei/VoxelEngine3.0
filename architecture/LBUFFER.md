# lBuffer — Per-Voxel Radiance Cache (as-built, Milestone B1)

The lBuffer is a GPU-resident, hash-indexed, LRU-managed cache mapping a world-space
voxel `(pos, level, version)` to a slot holding its normal, per-frame radiance
accumulators, and temporally-blended diffuse/specular channels. It is the substrate
for the irradiance-cache feedback and the per-voxel temporal denoiser
([PIPELINE.md](PIPELINE.md)). Source of truth: `src/shd/lbuffer.glsl` +
`src/renderer/renderer.{hpp,cpp}`.

> **Supersedes the earlier drafts.** Temporal accumulation is a per-voxel sample-capped
> **EMA** (not double-buffer SWAP). Channels are **RGB9E5** (HDR), not clamped u16. The
> table is now **flat and open-addressed** (B1.5, [CLAIM.md](CLAIM.md)) — there are **no
> buckets and no lock buffer**; the old `LBUFFER_BUCKETS`/128-slot-bucket + separate-mutex
> design and its retry machinery are gone.

---

## Constants (`renderer.hpp` ↔ `lbuffer.glsl`, keep in sync)

| Constant | Value | Notes |
|---|---|---|
| `LBUFFER_SLOTS_TOTAL` | 2²² = 4 194 304 | flat slot count, power of 2 (`& (N−1)` home slot); ≈ 2× peak visible voxels |
| `SLOT_DWORDS` | 16 | 64 B = 1 cache line |
| `LB_PROBE_LIMIT` | 32 | max linear-probe length before `NO_SLOT` |
| lBuffer size | `N·SLOT_DWORDS·4` = **256 MB** | slot data (binding 2); the *only* lBuffer SSBO |
| `ACCUM_SCALE` | 1024.0 | HDR fixed-point for u32 atomic accumulators |
| `FIREFLY_CLAMP` | 16.0 | per-sample radiance clamp |

**Cache-line alignment:** slot `s` is `s·16` DWORDs → every slot is 64-byte aligned (one
L2 line). The flat layout needs no inline lock word, so alignment is automatic.

---

## Slot layout (16 DWORDs = 64 B)

| DWORD | Field | Writer |
|---|---|---|
| 0 | `pos.x[0:15] \| pos.y[16:31]` | claim (key) |
| 1 | `pos.z[0:15] \| level[16:23] \| version[24:31]` | claim (key) |
| 2 | `timestamp` (LRU frame, = `frameTime` ms) | claim |
| 3 | `flag[0] \| spare[1] \| z[2:11] \| y[12:21] \| x[22:31]` (3×10-bit normal) | normal.comp |
| 4 | diffuse channel — **RGB9E5**, incident irradiance E/π | avg.comp |
| 5 | specular channel — **RGB9E5**, outgoing radiance | avg.comp |
| 6 | `N_diff[0:15] \| N_spec[16:31]` (EMA sample counts) | avg.comp |
| 7–9 | diffuse accumulator R,G,B (u32 HDR fixed-point) | accum (add) / avg (clear) |
| 10 | `pixelCount` (u32) | accum (add) / avg (clear) |
| 11–13 | specular accumulator R,G,B (u32 HDR fixed-point) | accum (add) / avg (clear) |
| 14 | **ReSTIR DI TEMPORAL reservoir** — light `pos.x[0:8]\|pos.y[9:17]\|pos.z[18:26]\|M[27:31]` (M==0 ⟹ none; depth ≤ 9) | restir |
| 15 | **ReSTIR DI TEMPORAL reservoir** — `W` (fp32 contribution weight) | restir |

> **Spatiotemporal reservoir is a *separate* parallel buffer (binding 6, 2 DWORDs/slot, 32 MB), not in
> this 64 B slot.** D14/D15 hold the per-frame **temporal** output (read by `restir_spatial` as a
> snapshot); the **spatial** pass writes the combined result to the parallel `resvSp[slot]`, which `shade`
> reads and the next frame's temporal folds back in (the feedback loop). `resvSp[slot]` is zeroed in
> `initSlot` alongside D4–D15. See [RESTIR.md](RESTIR.md) §E1.

- **Identity / key** = D0+D1. Empty slot = `D0==0 && D1==0` (a real leaf has `level≥1` so D1≠0).
- **Diffuse channel = E/π** (the cosine-sampled incoming radiance estimate, **pre-albedo**). So `resolve` and the feedback term are both `albedo·channel` with no stray π, and albedo-demodulated accumulation keeps edges crisp.
- **Normal** = 3×10-bit signed unit vector, flag at bit 0.

---

## Key + probe (`lbuffer.glsl`)

```glsl
uint voxelKey(uvec3 pos, uint level, uint version);   // nested lb_hash, & 0x7FFFFFFF (non-zero)
uint homeSlot(...) = voxelKey(...) & (LBUFFER_SLOTS_TOTAL - 1u);   // open-addressing home
uint slotBase(uint slot) = slot * SLOT_DWORDS;
uint probeLBuffer(pos, level, version);   // linear probe from home; stop at first D2==0 → slot or LB_NO_SLOT
```

`version` (8-bit node incarnation, [OCTREE.md](OCTREE.md)) disambiguates a voxel
edited off-screen and returned at the same `(pos,level)`, so its stale entry is not
reused (ages out via LRU).

---

## Claim protocol (`lbuffer_claim.comp`) — flat, lock-free, single dispatch

Full spec: [CLAIM.md](CLAIM.md). One thread per deduped voxel linear-probes from `homeSlot`
and takes a slot by `atomicCompSwap` on the **timestamp word (D2)** as a per-slot,
per-frame token — empty / match / evict all compete on that one word, so exactly one thread
wins per frame and losers advance the probe (no spin, no lock buffer, no CPU retry loop).
Eviction guard: only a slot with `D2 < frameStamp` is evictable (never one claimed this
frame). `initSlot` writes `claimedSlot` back to `uniqueList[origIndex].z`.

**Staleness decay (no extra storage):** on a MATCH re-claim the thread still holds the old
`d2` in-register; if `frameStamp − d2 > staleFrames` it drops the sample count to 1 so new samples
dominate fast. **B3 (settled): this reset now applies to BOTH the diffuse and specular counts,
under one shared window knob.** Specular needed it for view-dependence; diffuse needs it for
**dynamic** lighting (a removed light, or a geometry edit between a dark and a lit room — out-of-view
voxels otherwise pop with seconds-old light on revisit). The channel *value* is untouched; only the
EMA count drops, so a stale revisit re-converges fast with no pop-to-black. **No per-slot staleness
DWORD** — which keeps **D14/D15 free for the ReSTIR reservoirs** ([RESTIR.md](RESTIR.md)). *Watch-point:*
diffuse converges slower, so if the shared window churns diffuse, split into two windows later (a
tradeoff knob, §1D-clean — not a correctness hack).

---

## Accumulate + temporal blend

**accum.comp** (per virtual pixel) atomic-scatters HDR fixed-point samples:
```glsl
atomicAdd(data[sb+LB_PIXELS], 1u);
atomicAdd(data[sb+LB_DIFF_ACC+i], uint(clamp(diff[i],0,FIREFLY_CLAMP) * ACCUM_SCALE));   // i=0..2
atomicAdd(data[sb+LB_SPEC_ACC+i], uint(clamp(spec[i],0,FIREFLY_CLAMP) * ACCUM_SCALE));
```
*Overflow:* safe at the default `virtualScale` (≈130 K virtual pixels); very low
`virtualScale` + a screen-filling clamped voxel can saturate (rare; radiance ≈ [0,2]).

**avg.comp** (per voxel) — **sample-capped EMA**, the chosen temporal scheme:
```glsl
frameMean = accum / (pixels * ACCUM_SCALE);
channel   = (channel*N + frameMean*pixels) / (N + pixels);   // N weighted by this frame's coverage
N         = min(N + pixels, cap);                            // cap = emaDiffuse (long) / emaSpecular (short)
// then clear the per-frame accumulators (D7..D13, pixelCount)
```
Diffuse uses a long window (denoise hard); specular a short window (view-dependent →
forget fast under camera motion). Past the cap this is a bounded EMA with that window.

> **Why EMA, not double-buffer SWAP:** sampling is sparse and jittered, so a global
> time-based A→B swap would strobe voxels whose A buffer is under-filled at swap time.
> A per-voxel sample-capped EMA is adaptive per voxel and never strobes. Staleness control is
> in-register: `lbuffer_claim.comp` reads the old timestamp and decays the sample count past
> `specStaleFrames` (above) — *not* a return to double-buffering. It currently resets only
> specular; extending it to diffuse for dynamic lighting (B3) needs no extra storage either.

---

## Resolve readout

| renderMode | Output |
|---|---|
| `SHADING` | `(1-metallic)·albedo·unpack(diffuse) + unpack(specular) + (emissive ? emission : 0)`; `NO_SLOT` → flat albedo |
| `NORMAL` | `UnpackNormal(D3)·0.5+0.5` |
| `MATERIAL` / `OCTREE` / `VERSION` | from gbuffer |
| `CLAIM_AGE` / `LRU_OCCUPANCY` / `SAMPLES` | cache debug (timestamp age / probe hit / D6 sample count) |
| `VIRTUAL` / `SHADE` | virtual-gbuffer / raw 1-spp shade preview |

---

## C++ allocation (`renderer.cpp`)

**One** zero-initialised SSBO: the flat slot buffer (`LBUFFER_BYTES = N·64`, binding 2). It
does not resize (fixed `LBUFFER_SLOTS_TOTAL`). The old lock buffer (binding 9) and the
retry/retryCount buffers (bindings 5/6/7) were deleted with B1.5 ([CLAIM.md](CLAIM.md)).

## Reservoirs live in-slot (no second SSBO)

ReSTIR reservoirs reuse the freed slot DWORDs rather than a parallel buffer — which would
*add* VRAM (the lBuffer is already the memory pressure) and split one concept across two
buffers. **DI** fits in D14+D15 (`{y,W,M}`; `p_hat` recomputed) — **both free and settled** now
that B3 needs no per-slot staleness stamp (above). **GI** is reference-based (store the sample
voxel's key + `W`+`M`; re-fetch its radiance/normal from that voxel's own slot) and fits in ~3
DWORDs once D3 is oct-encoded (2×10-bit normal) and D6's sample counts are tightened (N_diff ≤10
bits, N_spec ≤6) — a packing change, **no new buffer**.

**Rejected/shelved for the reservoir budget (§1E):**
- **Moving the accumulators (D7–D13) to a parallel SSBO.** Conceptually right (they're transient,
  not persistent cache state), but indexed by **slot** the parallel buffer is the same `4M×7` DWORDs
  → *net-zero* memory. The only version that saves VRAM indexes by **visible-count** (≤ peak-visible,
  not slot total), which needs a slot→index back-reference DWORD *and* overflow handling. Real
  complexity for a VRAM win not needed today (depth 9–10 fit VRAM). Keep as a future VRAM-reduction
  refactor, not a reservoir-budget tool.
- **Chroma-RG (luma + 2-chroma, drop a channel).** Trades HDR colour fidelity on saturated emitters
  for bits RGB9E5 already packs into 32. Not worth it.

Full design: [RESTIR.md](RESTIR.md).
