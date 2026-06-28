# Milestone B1 — Per-Voxel Irradiance-Cache Path Tracer

> **STATUS: IMPLEMENTED & VERIFIED.** Full GI (skybox + emissive voxels) at interactive
> rates in a Cornell-box scene. Pipeline: primary · buildArgs · lbuffer_claim(×7
> ping-pong) · edit_mark · normal · **downscale · shade · accum · avg** · resolve ·
> final. As-built spec: [PIPELINE.md](PIPELINE.md); cache: [LBUFFER.md](LBUFFER.md).

**Goal:** turn the validated per-voxel cache ([MILESTONE_A.md](MILESTONE_A.md)) into a
lit image — a downscaled megakernel path tracer feeding a temporally-accumulated
per-voxel radiance cache, with multi-bounce GI via cache feedback. Lights = skybox +
emissive voxels (no NEE yet).

---

## Settled design decisions (and the alternatives rejected)

| Decision | Chosen | Rejected alternative |
|---|---|---|
| Shading domain | **Virtual framebuffer** (downscaled, `virtualScale`) | per-voxel dispatch (cheaper paths but no coverage weighting) |
| Path tracer | **Megakernel** (in-thread bounce) | wavefront ping-pong (premature; fat path state) |
| Multi-bounce | **Irradiance-cache feedback** — 1 traced bounce reads prev-frame cache ⇒ ∞ bounces over frames | explicit N-bounce per frame |
| Diffuse cache | **Irradiance E/π, pre-albedo** (RGB9E5) | final radiance (couples albedo into temporal signal) |
| Specular | **Stochastic GGX-VNDF**, short EMA window | IBL split-sum (inflexible for dynamic scenes) |
| Temporal | **Per-voxel sample-capped EMA** | double-buffer SWAP (strobes under sparse jitter) |
| Downscale jitter | **Per-pixel** random offset | global per-frame offset (coherent → structured coverage) |
| RNG seed | **World voxel identity** (`hash(pos,…)`) | screen pixel `vp` (bakes screen-periodic error) |
| Bounce/primary normal | cache probe → **radius-2 occupancy kernel** fallback | DDA face normal; slot-index-in-node (staleness + coupling) |
| `avg` dispatch | reuse **`uniqueVoxelList`** | separate `shadedVoxelList` + `buildAvgArgs` |
| Bucket lock | **separate SSBO** (binding 9) | inline DWORD-0 (breaks 64 B alignment, false-shares) |
| Slot | **16 DWORD, 128 slots/bucket** (~268 MB) | 5-DWORD (Milestone A) / 32 slots (overflowed) |

Energy: diffuse weighted by `(1-metallic)` at the composite/feedback points; emissive
added at the composite (view-independent — **not** in the diffuse channel, which would
multiply it by albedo via feedback). Ported `sampleGGXVNDF`/`ggxThroughput=F·G1(L)`
(Heitz 2018) from the pre-revamp code, **fixing its missing cosine-pdf division** (which
had made diffuse ~3× too dark).

---

## Debugging lessons (forward-useful only)

1. **Shaders deploy at CONFIGURE time.** CMake `file(COPY)` runs on configure, not build — after
   editing a shader, `cmake -S . -B build -G "MinGW Makefiles"` **then** `cmake --build build`;
   build alone does not redeploy. The copy must glob **only `src/shd/`**: a project-wide
   `GLOB_RECURSE` also matches CMake's own prior copies in `bin/<cfg>/shd` + `build/shd`, which
   collide on filename and non-deterministically overwrite the fresh shader with a frozen stale
   one — this caused a long "non-deterministic noise/pattern" hunt that was really a stale
   shader, not the sampler. Keep the glob scoped to source.
2. **Localize before changing code.** Debug render modes (MATERIAL/NORMAL/SAMPLES/LIGHTTREE/…)
   bisect the pipeline layer in seconds. Caveat: uniform-material/normal surfaces can't reveal a
   per-voxel *lighting* structure, so "MATERIAL/NORMAL clean" does not exonerate lighting. (B1's
   streaks were sample-count variance, not RNG — which motivated NEE/RIS.)

---

## Known limitation → B2 (built) → B2.5 (fix committed)

B1's per-voxel undersampling variance at high `virtualScale` was a sample-count problem,
attacked at the source by **NEE built as RIS** — now **built** (B2; light tree + RIS-NEE). The
*residual* lighting problem moved to the NEE shadow-ray **visibility test**: unbiased but noisy on
**solid clusters** and patterned on **flat emitters at grazing**. These are two *structural*
candidate-set problems with **committed B2.5 fixes** (strict test kept exact): enclosed-voxel
**surface-culling** ([LIGHTTREE.md](LIGHTTREE.md) §Surface-cull) for the cluster interior waste, and
**area-face sampling** ([RESTIR.md](RESTIR.md) §NEE visibility) for the grazing pattern. They target
*different* emitter shapes — surface-cull does nothing for a 1-voxel plate; area-face does nothing
for a solid blob's interior — so both ship together. Lower `virtualScale` / larger EMA still mitigate
the remaining sample-count variance.

## B2 → E direction (settled this session)

| Decision | Chosen | Rejected / corrected |
|---|---|---|
| Next focus | **NEE/RIS lighting quality** | paging/streaming (deferred — depth 9–10 fit VRAM) |
| Light storage | **sparse emissive-only light SVO**, CPU-maintained | power in main octree (bloats hot nodes); GPU visible-light list (lags) |
| Light sampling | **descend the tree** (power, +spatial later) | flat power **alias table** (power-only, can't be spatial; extra structure) |
| NEE shape | **build as RIS** (M candidates, WRS, 1 shadow ray, MIS) | single-light NEE (would be thrown away at ReSTIR) |
| MIS rule | weight **only the direct-emission term**; feedback term unweighted | MIS-weighting the whole `traceRadiance` return (double-count bug) |
| ReSTIR flavour | **world-space per-voxel DI → GI** | ReSTIR **PT** (needs path reservoir + shifts — contradicts the diffuse-cache/short-specular split); screen-space / hybrid per-pixel |
| DI vs EMA | DI emits a clean direct term that **feeds** the `E/π` channel | "reservoir **replaces** the EMA" (old ROADMAP E — different variance axes) |
| Reservoir storage | **in-slot** — DI in D14+D15; GI **reference-based** (~3 DWORDs) | parallel SSBO (adds VRAM, splits one concept) |
| Freed space | **D14+D15 free (settled — B3 needs no staleness stamp)**; oct-normal D3 + tight D6 reclaim ~1 more for GI | growing `SLOT_DWORDS`; accumulator-move (net-zero by slot) / chroma-RG (§1E) |
| GI scope | **diffuse + wide-glossy**, roughness-gated | resampling sharp/mirror specular (kept on the traced path) |
| `version` policy | **global counter** (confirmed in code since Milestone A) | — |

Full designs: [RESTIR.md](RESTIR.md), [LIGHTTREE.md](LIGHTTREE.md).

## B2.5 → B4 direction (settled this session)

| Decision | Chosen | Rejected / corrected |
|---|---|---|
| Order | **noise cleanup (B2.5) BEFORE MIS (B4)** — clean the candidate set, then combine/amortise | MIS or ReSTIR first (amortises garbage — §1E) |
| Cluster noise | **enclosed-voxel surface-cull** of the light tree (all 6 face-neighbours solid → excluded; exact, view-independent, O(depth)/edit) | the *toward-camera* surface cull (view-dependent — stays rejected); leaving interior voxels in (wasted `M`) |
| Grazing noise | **area-face sampling** — connect to a random point on the receiver-facing face (real `cosLight`) | point-on-voxel centre (the pattern's cause); a looser/biased visibility test (§1D) |
| Visibility test | **strict `hit.position==y`, unchanged** | any-emissive / distance-tolerance (bias) |
| Bounce emission | **stays OFF until B4 MIS** | re-enabling now (double-counts NEE's direct term) |
| Diffuse staleness (B3) | **shared in-claim stale-frames reset on both channels** — no per-slot stamp → D14/D15 free | a precise per-slot staleness DWORD (contends with DI) |
| Reservoir budget | **D14+D15 (DI); +1 via oct-D3 / tight-D6 (GI)** | accumulator-move (net-zero by slot; VRAM-only, deferred); chroma-RG (colour loss) |
| Scene import | **bake into the master octree** (reuses edit/flush → feeds light tree free) | instanced/transformed objects + TLAS (defer until rigid dynamic motion — §1E) |
| New philosophy | **§1E — complexity must justify its weight** (default-no, think 3×, stay ambitious in capability) | — |

## Render modes (resolve)

`SHADING` (default) · `OCTREE` · `MATERIAL` · `NORMAL` · debug `VERSION` · `CLAIM_AGE` ·
`LRU_OCCUPANCY` · `VIRTUAL` (downscaled gbuffer) · `SHADE` (raw 1-spp) · `SAMPLES`
(per-voxel EMA count).
