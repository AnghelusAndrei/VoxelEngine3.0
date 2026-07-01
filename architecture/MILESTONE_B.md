# Milestone B — Illumination

> **STATUS: BUILT & RUNNING.** A per-voxel irradiance-cache megakernel path tracer (B1) + lock-free
> claim (B1.5) + dynamic-light staleness (B2). Direct light = the diffuse bounce landing on emissive
> voxels; the per-voxel cache + temporal EMA accumulate it. As-built pipeline: [PIPELINE.md](PIPELINE.md);
> cache: [LBUFFER.md](LBUFFER.md). Next: ReSTIR GI ([RESTIR.md](RESTIR.md)).

## B1 — Settled design decisions

| Decision | Chosen | Rejected alternative |
|---|---|---|
| Shading domain | **Virtual framebuffer** (downscaled, `virtualScale`) | per-voxel dispatch (no coverage weighting) |
| Path tracer | **Megakernel** (in-thread bounce) | wavefront ping-pong (premature; fat path state) |
| Multi-bounce | **Irradiance-cache feedback** — 1 traced bounce reads prev-frame cache ⇒ ∞ bounces over frames | explicit N-bounce per frame |
| Direct light | **bounce hits an emissive voxel** (visibility built in) | NEE / shadow-ray light connection (self-occludes on voxel clusters — see [ROADMAP.md](ROADMAP.md) Rejected) |
| Diffuse cache | **Irradiance E/π, pre-albedo** (RGB9E5) | final radiance (couples albedo into the temporal signal) |
| Specular | **Stochastic GGX-VNDF**, short EMA window + adaptive à-trous | IBL split-sum (inflexible for dynamic scenes) |
| Temporal | **Per-voxel sample-capped EMA** | double-buffer SWAP (strobes under sparse jitter) |
| Downscale jitter | **Per-pixel** random offset | global per-frame offset (structured coverage) |
| RNG seed | **World voxel identity** (`hash(pos,…)`) | screen pixel `vp` (bakes screen-periodic moiré into the cache) |
| Normal | cache probe → **radius-2 occupancy kernel** fallback | DDA face normal; slot-index-in-node (staleness + coupling) |

Energy: diffuse weighted by `(1-metallic)` at the composite/feedback points; emissive added at the
composite (view-independent). Specular uses Heitz-2018 `sampleGGXVNDF` / `ggxThroughput = F·G1(L)` with
the cosine-pdf division (its absence had made diffuse ~3× too dark).

## B2 — Dynamic-light staleness

The diffuse cache goes stale when lighting changes (a removed light; a geometry edit between a dark and a
lit room → out-of-view voxels return with seconds-old light). Fix: the in-claim `staleFrames` reset drops
**both** EMA sample counts (diffuse + specular) under one shared window so fresh samples dominate fast;
the channel value stays (no black flash). No extra storage. The window is a responsiveness/convergence
tradeoff (§1D-clean), not a correctness knob. [LBUFFER.md](LBUFFER.md), [CLAIM.md](CLAIM.md).

## Forward-useful debugging lessons

1. **Shaders deploy at CONFIGURE time.** CMake `file(COPY)` runs on configure, not build — after editing
   a shader, `cmake -S . -B build -G "MinGW Makefiles"` **then** `cmake --build build`; build alone does
   not redeploy. Keep the copy glob scoped to `src/shd/` (a project-wide `GLOB_RECURSE` also matches prior
   copies in `bin/<cfg>/shd` + `build/shd` and non-deterministically overwrites the fresh shader with a
   stale one — a long "non-deterministic noise" hunt that was really a stale shader).
2. **Localize before changing code.** Debug render modes (MATERIAL/NORMAL/SAMPLES/…) bisect the pipeline
   layer in seconds. Caveat: uniform-material/normal surfaces can't reveal a per-voxel *lighting* bug.
3. **Verify on the CONVERGED image**, not a single 1-spp frame.

## Render modes (resolve)

`SHADING` (default) · `OCTREE` · `MATERIAL` · `NORMAL` · `VERSION` · `CLAIM_AGE` · `LRU_OCCUPANCY` ·
`VIRTUAL` (downscaled gbuffer) · `SHADE` (raw 1-spp) · `SAMPLES` (per-voxel EMA count) · `STEPS` (DDA cost).
