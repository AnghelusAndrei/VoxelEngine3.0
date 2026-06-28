# Rendering Pipeline Roadmap

> **State:** the per-voxel irradiance-cache path tracer (B1) + lock-free claim (B1.5) +
> emissive light tree with RIS-NEE (B2) are **built and running** at interactive rates. The
> active front is **lighting quality + correctness**. The immediate batch (**B2.5**) attacks RIS
> noise **at the source** — light-tree **surface-culling** + **area-face** NEE sampling — bundled
> with the **B3** diffuse-staleness fix; **MIS (B4)** and **ReSTIR DI/GI (E)** follow. As-built
> pipeline: [PIPELINE.md](PIPELINE.md); cache: [LBUFFER.md](LBUFFER.md); claim:
> [CLAIM.md](CLAIM.md); octree: [OCTREE.md](OCTREE.md); lighting design + open problems:
> [RESTIR.md](RESTIR.md) + [LIGHTTREE.md](LIGHTTREE.md); decision logs:
> [MILESTONE_A.md](MILESTONE_A.md) / [MILESTONE_B.md](MILESTONE_B.md).

## Staged overview

| Stage | Focus | Status |
|---|---|---|
| **0 — Foundation** | 64-bit SSBO octree, claim-bitfield dedup, majority material | ✅ done ([OCTREE.md](OCTREE.md)) |
| **A — Cache validation** | per-voxel claim / normal / edit-invalidation, no lighting | ✅ done ([MILESTONE_A.md](MILESTONE_A.md)) |
| **B1 — Illumination basis** | virtual-fb megakernel PT + irradiance-cache feedback + temporal EMA | ✅ done ([MILESTONE_B.md](MILESTONE_B.md)) |
| **B1.5 — Lock-free claim** | open-addressed voxel hash, CAS-on-timestamp | ✅ done ([CLAIM.md](CLAIM.md)) |
| **B2 — Light tree + RIS-NEE** | emissive-only SVO sampled by descent; NEE built as RIS (M candidates, WRS, 1 shadow ray) | ✅ built ([LIGHTTREE.md](LIGHTTREE.md), [RESTIR.md](RESTIR.md)) |
| **B2.5 — RIS noise cleanup** | light-tree **surface-cull** (enclosed-voxel, O(depth)/edit) + **area-face** NEE sampling | ⏳ **next** ([LIGHTTREE.md](LIGHTTREE.md), [RESTIR.md](RESTIR.md)) |
| **B3 — Dynamic lighting (diffuse staleness)** | extend the in-claim stale-frames reset to the diffuse sample count (shared window) | ⏳ **bundled with B2.5 — settled** |
| **B4 — MIS** | power-heuristic combine of NEE + the (re-enabled) bounce emission; light-tree reverse pdf-walk | ✅ built ([RESTIR.md](RESTIR.md)) |
| **E1.0 — ReSTIR DI (temporal)** | per-voxel reservoir `{y,W,M}` in D14/D15; temporal reuse; unshadowed reservoir + per-pixel V in shade | ✅ built ([RESTIR.md](RESTIR.md)) |
| **E1.1 — ReSTIR DI (spatiotemporal)** | per-voxel `restir_spatial` Z-combine → **parallel resvSp buffer** → folded into next temporal (the feedback loop); tangent sampler | ✅ built ([RESTIR.md](RESTIR.md)) |
| **E1.2 — curvature sampler** | replace tangent-plane neighbours with surface-snapped (±N) sampling so reuse works on spheres/curves | ⏳ **next** ([RESTIR.md](RESTIR.md)) |
| **E2 — ReSTIR GI** | directional indirect (diffuse + wide-glossy), reference-based reservoir | then ([RESTIR.md](RESTIR.md)) |
| **Scene import** | mesh / `.obj` voxelization → **bake into the master octree** | 🔲 fork with E2, after E1 |
| **C — LOD** | distance-based `leafDepth` coarsening (reads majority material) | planned |
| **D — Out-of-core** | octree paging — only when a scene exceeds VRAM | deferred |

*Ordering rationale (revised this session): attack RIS noise **at the source** before stacking
machinery on it. **B2.5** removes the two structural noise sources in the NEE candidate set —
occluded interior voxels of solid clusters (surface-cull) and the grazing point-on-voxel pattern
of flat emitters (area-face sampling) — so the set MIS/ReSTIR later amortise is already clean
(§1E: don't amortise garbage). **B3** diffuse-staleness rides along (a few lines, same in-claim
reset). **MIS (B4)** then combines the cleaned NEE with BSDF area-sampling — the principled fix
for the cluster/small-light asymmetry — and re-enables bounce emission (kept OFF until then to
avoid the double-count). **ReSTIR DI (E1)** amortises the survivors across frames/neighbours;
**scene import** and **ReSTIR GI (E2)** are the post-E1 fork. LOD/paging follow lighting: depth
9–10 fit VRAM ([OCTREE.md](OCTREE.md) §Capacity), so paging buys zero image quality until a real
scene exceeds it. ReSTIR PT is rejected ([RESTIR.md](RESTIR.md)).*

---

## B2 — Light tree + RIS-NEE ✅ built

Emissive-only SVO mirroring the octree, maintained **O(depth) per edit** via the octree's
`onLeafChanged` hook, sampled by power-weighted descent ([LIGHTTREE.md](LIGHTTREE.md)).
`shade.comp` draws `RIS_M` candidate lights, weighted-resamples to one winner (WRS), and casts
**one** strict shadow ray ([RESTIR.md](RESTIR.md)). The estimator is **unbiased**; its residual
noise is two *structural* problems in the candidate set, attacked next (B2.5).

## B2.5 — RIS noise cleanup (next)

Two parameter-free fixes (§1D), bundled because both live on the NEE / light-tree path. They
attack **different** noise sources — know which one your scene has:

**1. Surface-cull the light tree → solid clusters.** An emissive voxel whose **6 face-neighbours
are all solid** (emissive or not) is fully enclosed: it radiates nothing externally, so it is
**excluded from the light tree**. This deletes the always-occluded interior candidates the descent
otherwise wastes `M` budget on (the strict test correctly returns V=0 → pure variance). The cull
is **exact and view-independent** — distinct from the toward-camera surface cull
[LIGHTTREE.md](LIGHTTREE.md) previously rejected (a "back sphere" voxel keeps air on its back face
→ stays sampleable). **O(depth)/edit**, riding `onLeafChanged`; full algorithm + maintenance state
machine in [LIGHTTREE.md](LIGHTTREE.md) §Surface-cull. *Helps solid blobs; does **nothing** for a
1-voxel-thick plate — every plate voxel has air on two faces, so none is enclosed. The plate's
noise is the grazing pattern, fixed by (2).*

**2. Area-face NEE sampling → flat emitters at grazing.** Connect to a **uniform random point on
the chosen voxel's receiver-facing face**, with that face's real normal (→ the light-side cosine
the center-point integrand omits). Varying the connection point dissolves the structured grazing
pattern into unbiased noise. Start with the **dominant face** (§1E); upgrade to projected-area-
weighted 3-face only if the residual shows. **As built:** each candidate area-samples its own face
point and evaluates `p̂` there (exactly unbiased); the winner's shadow ray aims at it. Full spec: [RESTIR.md](RESTIR.md)
§NEE visibility.

**Hard constraint:** the diffuse bounce stays `includeEmission=false` until **MIS (B4)** —
area-face + surface-cull make NEE-alone clean enough in the interim; re-enabling bounce emission
without MIS double-counts direct light.

## B3 — Dynamic lighting · diffuse staleness (settled, bundled with B2.5)

*Resolves the earlier "open — NOT free" status.* The in-claim `specStaleFrames` reset currently
refreshes only the **specular** channel; the **diffuse** irradiance cache also goes stale under
dynamic lighting (a removed light, or a geometry edit between a dark and a lit room → out-of-view
voxels pop with seconds-old light on revisit). **Settled fix:** extend the *same* in-register
stale-frames reset to the **diffuse** sample count, under **one shared window knob**. The channel
*value* is untouched — only the EMA count drops, so a stale revisit shows the last value and
re-converges fast (no pop-to-black). The window is a responsiveness/convergence tradeoff
(§1D-clean), not a correctness knob. **No per-slot staleness DWORD** — which **frees D14/D15 for
the ReSTIR reservoirs** and resolves the old slot-budget collision ([LBUFFER.md](LBUFFER.md),
[RESTIR.md](RESTIR.md)). Edit-driven epoch invalidation stays rejected (misses geometry edits;
thrashes on frequent edits). *Watch-point:* diffuse converges slower than specular, so if the
shared window churns diffuse, split into two windows later (still a tradeoff knob).

## B4 — MIS ✅ built

The diffuse bounce is back to `includeEmission=true`; its direct-emission term and the NEE term are
**power-heuristic combined** so a bounce landing on a light is merged with NEE, not double-counted.
This is the principled fix for the cluster/small-light asymmetry **and** for the dominant-face
under-count (B2.5): the BSDF technique hits whatever face is in its path — including the non-dominant
faces NEE drops — and where NEE's pdf is zero the heuristic gives the bounce full weight.

As built (`shade.comp`, `lighttree.glsl`):
- **`lightTreePdf(leafPos)`** — a root→leaf reverse walk of the light tree (`Π childPower/total`,
  == that leaf's forward `pdfDisc`) giving the probability the descent *would* have sampled a
  bounce-hit voxel.
- **`lightSolidAnglePdf` — one canonical centre-based `p_nee`** (`pdfDisc·d²/(A·cosLight)`, voxel
  centre + dominant face) used **identically** for the NEE winner and the BSDF hit. This is the
  partition-of-unity requirement: computing `p_nee` two different ways (e.g. sampled-point vs centre)
  makes `w_nee + w_bsdf ≠ 1` → bias, concentrated exactly on close/intense off-axis lights. `p_bsdf`
  is `cosθ/π` in both places. The stochastic face point still drives the shadow ray and the RIS
  contribution — only the MIS *weight* uses centre geometry.
- **Only the direct-emission term is weighted**; the cached-diffuse feedback is unweighted (the
  double-count trap — [RESTIR.md](RESTIR.md) §MIS rule). Specular keeps emission unweighted (no NEE
  on specular). `lightTreePdf == 0` (leaf not sampleable) → `w_bsdf = 1` (correct, consistent).

## E — Per-voxel ReSTIR DI / GI (world-space, in-slot)

Full design: [RESTIR.md](RESTIR.md). Reservoir work dispatches over `uniqueVoxelList`; the lBuffer
*is* the world-space reservoir grid, so reuse is per-voxel, not screen-space.

**E1.0 (DI, temporal) ✅ built.** A new per-voxel `restir.comp` (between `normal` and `downscale`)
draws RIS_M unshadowed candidates, weighted-resamples to one, and **temporally merges this voxel's
previous-frame reservoir** (it persists in the slot — `initSlot` only runs on empty/evict claims, so
a MATCH re-claim keeps it; a stale revisit zeroes M). Stores `{y,W,M}` in D14/D15 (`W = wSum/(M·p̂)`,
canonical merge weight `p̂_cur·W·M` for both inputs, M-capped by `restirMCap`). `shade.comp` reads
the reservoir and adds `f·W·w_nee` evaluated at the light **centre**, with a per-pixel shadow ray to
that centre. **Centre-consistency is load-bearing:** `f` and `W`'s target `p̂` must use the *same*
geometry or a per-pixel geometry ratio survives and spikes on close lights → fireflies that grow with
M (the temporal-reuse bug, fixed). The grazing-pattern cost of centre-point visibility is covered by
the BSDF technique under MIS. RNG mixes `frameIndex` so each frame draws fresh candidates. **D14
packs pos (9 bits/axis) | M (5 bits) → depth ≤ 9** (renderer warns above that). *(Area-face partial
visibility is dropped here; re-add via a deterministic per-(voxel,frame) point shared by both passes
only if the grazing pattern visibly returns — §1E, measure first.)*

**E1.1 (DI, spatiotemporal) ✅ built.** The combine is a **per-voxel `restir_spatial.comp`** (between
`restir` and `downscale`) that reads the **slot D14/D15 temporal snapshot** of self + `spatialSamples`
neighbours (random tangent-plane offsets, each keyed by `octLeafAt` for its `version`) and writes the
combined reservoir to a **parallel buffer `resvSp`** (binding 6, 2 DWORDs/slot, 32 MB). The per-frame
flow closes the **feedback loop** — the real ReSTIR accelerator:
`resvSp(N-1) → temporal → slot D14/D15 → spatial → resvSp(N) → shade & temporal(N+1)`.
**Race-free by construction:** spatial *reads* the slot and *writes* `resvSp` (disjoint), so no thread
ever sees another's spatial output → no `M`-inflation (the bias an in-place per-voxel pass would hit).
The combine is **Bitterli `Z = Σ M_i·[p̂_i(y)>0]`** (only participants whose own surface faces the chosen
light normalise — no rejection knob, §1D); `M` clamped to the 5-bit field on store. **Honest caveat:** the
loop reintroduces a small **bounded correlation bias** (q→neighbour→q across frames) capped by
`restirMCap` — standard ReSTIR, a §1D responsiveness tradeoff, *not* "unbiased." **Lifecycle (must-fix):**
`resvSp[slot]` is zeroed in `lbuffer_claim`'s `initSlot` on claim/evict — else a reused slot would serve a
prior voxel's light on disocclusion. **Curvature limit:** tangent-plane neighbours leave a *curved*
surface at radius, so reuse helps flats but not spheres yet — **E1.2** swaps in a surface-snapped sampler.

**E2 (GI) — later.** Resamples the indirect bounce (reference-based; diffuse + wide-glossy only).
**Slot budget:** DI = D14 (`pos|M`) + D15 (`W`); GI reclaims ~1 more DWORD via oct-encoding D3 +
tightening D6 ([LBUFFER.md](LBUFFER.md)).

## Scene import — mesh / `.obj` voxelization (bake into the master octree)

**Goal:** load external / reference scenes and voxelize them into the octree to validate lighting
on ground-truth geometry rather than the procedural Cornell box. **Settled (§1E):** voxels are
**baked into the master octree** at import via the existing `insert` / `flushEdits` path — which
feeds the light tree for free through `onLeafChanged`, with zero new runtime structure.
**Instanced / transformed voxel objects (a TLAS of octrees + transformed-ray DDA) are deferred** —
they do not justify their complexity until rigid *dynamic* motion is a real requirement, and the
single-octree DDA cannot transform rays today. **Open (post-E1 fork with E2):** voxelization
method (surface vs solid fill; conservative-raster vs mesh-SDF), material/UV → voxel-material
mapping, target resolution vs octree depth.

## C — LOD / distance coarsening

`Octree::insert` already takes `leafDepth`. A CPU policy inserts far voxels at reduced depth;
the DDA terminates earlier (the primary-ray cost driver), and the majority `reprMaterial`
([OCTREE.md](OCTREE.md)) gives coarse nodes a plausible colour. The lBuffer key encodes
`level`, so a coarse voxel and the fine voxels it covers are distinct cache entries.

## D — Out-of-core paging (deferred until VRAM is the wall)

Depth 9–10 fit VRAM and the `next` ceiling is retired, so paging buys **zero image quality** —
it waits until a target scene actually exceeds VRAM. When it lands: partition the octree into
fixed-size pages on disk; keep the hot set GPU-resident; `primary.comp` detects a page fault
(sentinel slot) → feedback buffer → CPU async load → `glBufferSubData`. Preserve the
`flushEdits` / `resizeDataIfNeeded` CPU→GPU contract.
