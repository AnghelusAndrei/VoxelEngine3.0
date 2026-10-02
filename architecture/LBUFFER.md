# lBuffer — Per-Voxel Radiance Cache

A flat, open-addressed, LRU hash table holding one persistent slot per visible voxel: its normal, its
temporally-blended diffuse and specular irradiance, and the statistics that drive convergence. It is
the substrate the path tracer feeds back into, and the reason bounces compound across frames.

Frame flow: [PIPELINE.md](PIPELINE.md). Node format: [OCTREE.md](OCTREE.md).

## Sizing

| Constant | Value | Meaning |
|---|---|---|
| `LBUFFER_SLOTS_TOTAL` | 2²² = 4.19 M | power of two, so `home = hash & (N-1)` |
| `SLOT_DWORDS` | 16 | 64 B = one cache line |
| `LB_PROBE_LIMIT` | 32 | max linear probe before `LB_NO_SLOT` |
| total | **256 MB** | binding 2, the only lBuffer SSBO |
| `ACCUM_SCALE` | 1024.0 | HDR fixed-point for the u32 atomic accumulators |
| `FIREFLY_CLAMP` | 16.0 | per-sample clamp before accumulation |

Slot `s` starts at DWORD `s·16`, so every slot is 64-byte aligned: one line, one fetch. Size ≈ 2×
peak visible voxels keeps the load factor ≤ 0.5 and the average probe ≈ 1.5.

Mirrored in `renderer.hpp` (`LBUFFER_*`) and `shd/constants.glsl` — keep in sync.

## Slot layout (16 DWORDs)

| DWORD | Field | Writer |
|---|---|---|
| 0 | `pos.x[0:15] \| pos.y[16:31]` | claim (key) |
| 1 | `pos.z[0:15] \| level[16:23] \| version[24:31]` | claim (key) |
| 2 | `timestamp` — monotonic `frameStamp`; **0 ⟺ pristine-empty** | claim |
| 3 | `flag[0] \| spare[1] \| z[2:11] \| y[12:21] \| x[22:31]` (3×10-bit normal) | normal.comp |
| 4 | diffuse channel — **RGB9E5**, incident irradiance E/π | avg.comp |
| 5 | specular channel — **RGB9E5**, outgoing radiance | avg.comp |
| 6 | `N_diff[0:15] \| N_spec[16:31]` — EMA sample counts | avg.comp |
| 7 | `listIndex` — this voxel's `uniqueVoxelList` index this frame | claim |
| 8 | EMA **mean** of log-luminance — float32 **bit pattern** | avg.comp |
| 9 | EMA **variance** of log-luminance — float32 **bit pattern** | avg.comp |
| 10 | EMA **drift** of log-luminance — float32 **bit pattern** | avg.comp |
| 11 | **filtered** diffuse — RGB9E5 ([FILTER.md](FILTER.md)) | filter.comp |
| 12 | **filtered** specular — RGB9E5 | filter.comp |
| 13–15 | free | |

D11/D12 are what `holefill` publishes, what `resolve` composites, and what `shade`'s bounce reads
back. D4/D5 stay the raw EMA and are the filter's **only** input — a filter that read a neighbour's
D11 would compose blur with blur and grow its support without bound ([FILTER.md](FILTER.md)).

- **Identity** = D0+D1. A real leaf has `level ≥ 1`, so `D1 == 0` cannot be a live key.
- **D8/D9 are floats in a `uint[]` buffer.** They must be read with `uintBitsToFloat` and written
  with `floatBitsToUint`. A plain cast converts the integer *numerically* and silently yields values
  around 1e9.
- Every access goes through the `LB_*` names. The layout has shifted more than once and a hardcoded
  offset reads a different field with no error.

### Per-frame accumulators live outside the slot

`accumDiffuse` / `accumSpecular` (bindings 6/7) are `uvec4[]` **parallel to `uniqueVoxelList`**:
`xyz` = HDR fixed-point sum, `accumDiffuse.w` = `pixelCount`. Both are sized to the virtual-pixel
count and allocated with the list.

Keeping them out of the slot does two things: it leaves D10–D15 free, and it moves `accum`'s seven
`atomicAdd`s off a 64 B line that `shade`, `holefill` and `resolve` all want to *read* the same frame
— atomics force exclusive ownership of a line. The accumulators are indexed in list order, which is
far more coherent than hash order.

`claim` writes the link into **D7**, `shade` relays it into the virtual gbuffer's spare bits, and
`accum` reads it from the texel it was already loading — so `accum` performs no lookup, no hash and
no octree read at all.

> `uvec3` is a lie in std430: an array element pads to 16 B. Both accumulators are `uvec4`, and the
> host allocates 4 DWORDs per entry. `glBufferData(..., NULL, ...)` leaves the store undefined, so
> both are cleared on allocation — `avg` reads `pixelCount` before anything writes it on the first
> frame after every resize, and garbage there blends into the EMA and sticks.

---

## Key and read paths

```glsl
uint voxelKey (pos, level, version);              // nested lb_hash, & 0x7FFFFFFF (non-zero)
uint homeSlot (pos, level, version);              // voxelKey & (N-1)
uint probeLBuffer (pos, level, version);          // linear probe from home; stop at first D2 == 0
uint lookupLBuffer(pos, level, version, nodeLo);  // O(1): home + baked offset, key-verified
uint findLBuffer  (pos, level, version, nodeLo);  // lookup, falling back to probe
```

`version` (8-bit node incarnation) is the third key component. It disambiguates *incarnations*: a
voxel edited off-screen and returning at the same `(pos, level)` hashes differently, so its stale
entry is not reused and ages out via LRU.

`probeLBuffer` stops at the first pristine-empty slot — a key always sits before the first empty in
its run, because insert claims the first empty and eviction overwrites rather than deletes.

**No pass calls `probeLBuffer` directly**; it survives as `findLBuffer`'s fallback. The choice per
call site follows from *when* the pass runs and *which* voxels it reaches:

| Call site | Reads | Path | Why |
|---|---|---|---|
| `resolve.comp` | primary voxel, per pixel | `lookupLBuffer` | hottest path; a miss is just a hole the fill absorbs |
| `shade.comp` › primary voxel | virtual-gbuffer voxel | `lookupLBuffer` | runs after claim, over exactly the set it just baked |
| `holefill.comp` | its own virtual texel | `lookupLBuffer` | same set, same frame |
| `shade.comp` › `traceRadiance` | **bounce** target (D11) | `findLBuffer` | an arbitrary, often off-screen voxel |
| `filter.comp` › world tap | tangent-plane neighbour | `findLBuffer` | same: arbitrary, often off-screen |
| `filter.comp` › screen tap | its own virtual texel's voxel | `lookupLBuffer` | same set, same frame |
| `edit_mark.comp` | neighbours of an edit | `findLBuffer` | mostly off-screen, and it runs right after an edit wiped the dirty range |

The asymmetry that decides it is failure *cost*, not hit rate: `accum` losing a sample recurs and
self-corrects every frame, while `edit_mark` losing a flag is one-shot — a stale normal that nothing
retries.

---

## Claim — lock-free, single dispatch

One thread per deduped voxel. Ownership is taken by `atomicCompSwap` on the **timestamp word (D2)**,
used as a per-slot per-frame token. Empty / match / evict all CAS that same word, so exactly one
thread wins and losers advance the probe. No spin, no lock buffer, no CPU retry loop.

```
h = homeSlot(key);  best = -1;  bestTs = MAX
for i in 0..LB_PROBE_LIMIT:
    s  = (h + i) & (N-1);  d2 = data[s].D2

    if data[s].D0 == key0 && data[s].D1 == key1:              # MATCH
        if CAS(data[s].D2, d2, frameStamp) == d2:
            age = frameStamp - d2                            # two staleness windows:
            if staleViewIndep && age > staleViewIndep: nDiff = 0   #   diffuse — scene changed
            if staleViewDep   && age > staleViewDep:   nSpec = 0   #   specular — camera moved
            claimed = s; break
        continue                                             # evicted under us → keep probing

    if d2 == 0:                                              # EMPTY (pristine)
        if CAS(data[s].D2, 0, frameStamp) == 0:
            write key; initSlot(s); claimed = s; break
        continue

    if d2 < frameStamp && d2 < bestTs: bestTs = d2; best = s  # evictable candidate

if !claimed && best >= 0:                                     # EVICT the stalest in the window
    if CAS(data[best].D2, bestTs, frameStamp) == bestTs:
        write key; initSlot(best); claimed = best
```

Safety rests on two pipeline invariants: `uniqueVoxelList` is deduplicated, so there is exactly one
thread per key and no concurrent same-key insert; and `frameStamp ≥ 1`, so `D2 == 0` is an
unambiguous pristine-empty sentinel. The eviction guard `d2 < frameStamp` means a slot claimed *this*
frame can never be evicted by another voxel in the same frame.

`initSlot` sets `NormalUpdateFlag` (D3) and zeroes D4–D15. The winner writes `claimedSlot` back to
`uniqueVoxelList[gid].z` and its `listIndex` to D7 — **once, after the claim resolves**, not inside
each branch: the evict path in particular must refresh it, since the slot still holds the previous
occupant's index and `accum` would otherwise scatter this voxel's radiance into that voxel's
accumulator. Eviction overwrites and never deletes, so probe chains stay intact.

### Slot-offset baking — the O(1) read path

The probe above runs once per *visible voxel* per frame. The read path runs once per *pixel*. So the
winner also writes its probe distance into the octree node:

```glsl
off = (claimed - home) & (N - 1);
if (off <= NODE_SLOTOFF_MASK) {                        // guard: LB_PROBE_LIMIT raised past 32
    lo    = octree.nodes[vid].x;                       // vid rides in the gbuffer's .w
    newLo = (lo & ~(MASK << SHIFT)) | (off << SHIFT);  // node Lo bits 27..31
    if (newLo != lo) octree.nodes[vid].x = newLo;       // skip the write when unchanged
}
```

`resolve` then reaches the slot with one node read plus one hash — no chain walk, and the node id was
already in the gbuffer so there is no traversal either. Node ids are spatially coherent (siblings are
adjacent in the pool), unlike the hash-scattered slots.

**This needs no validity bit.** `lookupLBuffer` verifies the full key on the slot's own cache line —
the same 64 B it is about to read the channels from, so the check is free. Every way the offset can be
wrong lands on a slot whose key disagrees:

| Case | Offset read | Result |
|---|---|---|
| never claimed | 0 (node initialised zero) | home slot — hits only if the voxel is at home |
| **any** CPU upload of the node | 0 | same |
| voxel evicted, another key owns the slot | stale | key mismatch |
| garbage from a recycled node | masked into range | key mismatch |

All of them return `LB_NO_SLOT`, the ordinary hole state the fill absorbs. Offset 0 is unambiguous:
it means *at home* exactly when the key agrees.

> **An octree upload wipes offsets in bulk.** The offset lives only in VRAM — the CPU mirror never
> carries it, because only `lbuffer_claim` writes those bits. So `flushEdits`' `glBufferSubData` over
> `[dirtyMin, dirtyMax)` zeroes it for **every node in that range**, and a full `glBufferData` zeroes
> all of them. A visible voxel is re-baked by the next claim; an off-screen one is not until it
> becomes visible, which is why the passes that reach arbitrary voxels use `findLBuffer`.

The claim pass binds the octree SSBO **coherent** and read-write for this; every other pass keeps it
`readonly`.

---

## Accumulate and temporal blend

`accum` scatters each virtual pixel's firefly-clamped samples into the parallel accumulators. `avg`
then folds one frame per voxel:

```glsl
frameMean = sum / (pixels · ACCUM_SCALE)
M' = (M·N + frameMean·pixels) / (N + pixels)          // coverage-proportional
N' = min(N + pixels, window)
```

Coverage-proportional: a voxel sampled by many virtual pixels this frame moves further than one
sampled once. Past the cap this is a bounded EMA with that window.

> Not a double-buffer A→B swap: sampling is sparse and jittered, so a global time-based swap strobes
> voxels whose A buffer is under-filled at swap time. A per-voxel sample-capped EMA is adaptive per
> voxel and never strobes.

### Variance-adaptive window

`window` is not a constant. It is derived from the voxel's own temporal variance:

```glsl
window = clamp(nDiffMax / (v + VAR_WINDOW_EPS), VAR_WINDOW_MIN, LB_COUNT_MAX)
```

So `emaDiffuse` is the window **at unit variance** — a reference point, not a cap. A converged voxel
(`v → 0`) earns up to 100× it and heavy smoothing; one whose light is moving gets a short window and
adapts fast. The `LB_COUNT_MAX` clamp is load-bearing: the count is stored in 16 bits, and at
`nDiffMax = 1024` the unclamped window is 102400, which wraps to 36864 and makes a converged voxel
*less* smoothed than a noisy one.

### The variance tracker (D8/D9)

One observation per frame — this frame's mean, as log-luminance. That is well defined even when a
voxel got a single virtual sample, which most do; a within-frame variance would not be.

```glsl
lframe = log(max(luminance(dframe), LUM_FLOOR))
if (nd == 0) { m = lframe; v = 0; }                   // seed, no history to blend
else {
    av    = clamp(pixels / (nd + pixels), VAR_ALPHA_MIN, VAR_ALPHA_MAX);
    delta = lframe - m;
    m    += av * delta;
    v     = max((1 - av) * (v + av * delta * delta), 0);
}
```

Four things here are deliberate:

- **Incremental (Welford-style), never `E[x²] − E[x]²`.** The subtraction form cancels
  catastrophically once the mean is large relative to the spread.
- **Log-luminance.** `FIREFLY_CLAMP` truncates the linear tail, so linear variance mostly measures
  "did this hit the clamp". Log is also scale-invariant, so one threshold works across the scene.
- **`LUM_FLOOR` is not a `log(0)` guard.** An unlit voxel is common and legitimate; without a floor
  its log runs to `-inf` and poisons the EMA with NaN.
- **`av` is clamped at BOTH ends.** The ceiling keeps a heavily-covered voxel from collapsing the
  update to exactly zero (at `av == 1`, `v = (1-av)(…) = 0`). The floor is what stops a *deadlock*:
  the irradiance alpha is `pixels/(n+pixels)`, so a voxel that accumulated a large `n` while close and
  then went distant (1 px of coverage, n = 3000) has a ~3000-frame variance memory — it can never
  observe that its light changed, so `v` stays ~0, so its window stays huge, so it never adapts. Small
  distant voxels are thin on screen, which turns that deadlock into visible streaks when the camera
  moves after sitting still.

The escape chain is: `v` rises → `window` shrinks → `N' = min(N + pixels, window)` collapses `N` in
one frame → the irradiance alpha jumps back up. The window cap is the escape hatch; the variance just
has to be able to move.

**Consumers.** `holefill` turns `v/n` into a per-tap confidence for `resolve`'s gather (§ PIPELINE).
`avg` drives its own window from it. `VARIANCE` displays `sqrt(v)` — standard deviation in log units —
because variance itself spans orders of magnitude between a converged wall and a contact shadow.

### Staleness

`staleViewDep` / `staleViewIndep` reset a channel's EMA **count** on a stale re-visit, never the
channel value, so a stale voxel re-converges from its old value instead of flashing black. With the
variance-adaptive window owning convergence, `staleViewIndep` is effectively an *eviction* control
rather than a freshness gate — diffuse irradiance is view-independent and only goes wrong when the
scene does.

---

## Host allocation

```cpp
glGenBuffers(1, &lBufferSSBO);
glBufferData(GL_SHADER_STORAGE_BUFFER, LBUFFER_BYTES, NULL, GL_DYNAMIC_DRAW);
glClearBufferData(...);                    // zero => every slot pristine-empty
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_LBUFFER_BINDING, lBufferSSBO);
```

Allocated once, bound persistently, never resized. The zero-fill is what makes `D2 == 0` mean
pristine-empty on frame one.

The payload is D0–D9. `SLOT_DWORDS` stays 16 because that is exactly one 64 B cache line: 12 would
be 48 B and slots would straddle lines, costing two fetches where the layout exists to guarantee
one. D10–D15 are free for fields that want to ride that same fetch.
