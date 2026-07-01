# Rendering Pipeline Roadmap

> **State:** the foundation (SSBO octree, lock-free per-voxel claim, dedup) and a **per-voxel
> irradiance-cache megakernel path tracer** (cache + bounce + temporal EMA) are built and running
> at interactive rates — this is the current, best-performing renderer. **Direct light comes from
> the diffuse bounce landing on emissive voxels; the per-voxel cache accumulates it over frames.**
> The active front is **denoising / faster convergence via ReSTIR GI** (reservoir reuse of bounce
> samples). As-built pipeline: [PIPELINE.md](PIPELINE.md); cache: [LBUFFER.md](LBUFFER.md); claim:
> [CLAIM.md](CLAIM.md); octree: [OCTREE.md](OCTREE.md); illumination + GI plan: [RESTIR.md](RESTIR.md).

## Staged overview

| Stage | Focus | Status |
|---|---|---|
| **0 — Foundation** | 64-bit SSBO octree, claim-bitfield dedup, majority material | ✅ done ([OCTREE.md](OCTREE.md)) |
| **A — Cache validation** | per-voxel claim / normal / edit-invalidation, no lighting | ✅ done ([MILESTONE_A.md](MILESTONE_A.md)) |
| **B1 — Illumination basis** | virtual-fb megakernel PT: cosine diffuse + GGX specular bounce, irradiance-cache feedback, temporal EMA | ✅ done ([MILESTONE_B.md](MILESTONE_B.md)) |
| **B1.5 — Lock-free claim** | open-addressed voxel hash, CAS-on-timestamp | ✅ done ([CLAIM.md](CLAIM.md)) |
| **B2 — Dynamic-light staleness** | in-claim stale-frames reset on diffuse + specular EMA counts (one shared window) | ✅ done ([LBUFFER.md](LBUFFER.md)) |
| **GI — ReSTIR GI** | spatiotemporal reservoir reuse of bounce samples (denoise direct emission + 1-bounce indirect) | ⏳ **next** ([RESTIR.md](RESTIR.md)) |
| **Scene import** | mesh / `.obj` voxelization → bake into the master octree | 🔲 then |
| **C — LOD** | distance-based `leafDepth` coarsening (reads majority material) | planned |
| **D — Out-of-core** | octree paging — only when a scene exceeds VRAM | deferred |

## B1 — Illumination basis (current best)

A downscaled **virtual framebuffer** is shaded by a **megakernel path tracer**: one cosine diffuse
ray + one GGX-VNDF specular ray, each a single bounce that reads the *previous frame's* per-voxel
cache at the hit (**irradiance-cache feedback** ⇒ effectively unbounded bounces over frames). A
bounce that lands on an **emissive voxel** returns its emission — that is the direct light, found by
the bounce's built-in visibility (no shadow ray). Per-frame samples scatter into per-voxel
accumulators and fold into the slot's channels via a per-voxel **EMA**; `resolve` composites at full
resolution. Skybox + emissive voxels are the lights. As-built spec: [PIPELINE.md](PIPELINE.md).

## B2 — Dynamic-light staleness (done)

A slot unseen for > `staleFrames` may hold light that no longer matches the scene (a removed light;
a geometry edit between a dark and a lit room). On a stale re-visit the in-claim reset drops **both**
EMA sample counts (diffuse + specular) to ≤1 so fresh samples dominate fast; the channel *values*
stay, so no black flash. The window is a responsiveness/convergence tradeoff (§1D-clean), needing no
extra storage. [LBUFFER.md](LBUFFER.md), [CLAIM.md](CLAIM.md).

## GI — ReSTIR GI (next)

The cache+EMA already denoises temporally; the missing piece is **spatial** reuse so convergence is
fast for the hard cases (newly-disoccluded regions, rare bounce hits on small/occluded emitters). The
chosen design is **ReSTIR GI on the bounce samples**: a reservoir per voxel holds *the bounce hit + the
radiance leaving it toward us*, reused spatiotemporally. A bounce that hits an emitter stores its
**emission** (direct light); a bounce that hits a lit surface stores its **cached radiance** (indirect)
— **one reservoir denoises both.** No NEE, no light list, no shadow-ray connection — the sample is
found by the bounce's own visibility, so the firefly-via-self-occlusion class is gone. The reservoir
is **reference-based** (store the hit voxel's key; re-fetch its cached radiance/normal from its own
slot) and lives in the free slot DWORDs (D14/D15). Full design + the one new cost (the reconnection
Jacobian): [RESTIR.md](RESTIR.md). *Known limit:* tiny/bright/isolated point lights — nothing
guarantees a bounce finds them; this engine's content is emissive **clusters**, which the bounce finds
easily, so it is a non-issue (escape hatch: greedy-mesh emissives into quad area lights — only if ever
needed).

## Scene import — mesh / `.obj` voxelization

Load external / reference scenes and voxelize them into the **master octree** via the existing `insert`
/ `flushEdits` path (reuses the edit pipeline, no new runtime structure). Instanced / transformed voxel
objects (a TLAS of octrees + transformed-ray DDA) are **deferred** — the single-octree DDA can't
transform rays and it doesn't justify its complexity until rigid dynamic motion is a real requirement.
Open: voxelization method (surface vs solid fill), material/UV → voxel-material mapping, target depth.

## C — LOD / distance coarsening

`Octree::insert` takes `leafDepth`. A CPU policy inserts far voxels at reduced depth; the DDA
terminates earlier (the primary-ray cost driver), and the majority `reprMaterial` ([OCTREE.md](OCTREE.md))
gives coarse nodes a plausible colour. The cache key encodes `level`, so a coarse voxel and the fine
voxels it covers are distinct cache entries.

## D — Out-of-core paging (deferred until VRAM is the wall)

Depth 9–10 fit VRAM, so paging buys **zero image quality** until a target scene exceeds it. When it
lands: partition the octree into fixed-size pages on disk; keep the hot set GPU-resident; `primary.comp`
detects a page fault → CPU async load → `glBufferSubData`. Preserve the `flushEdits` CPU→GPU contract.

---

## Rejected — NEE / light-tree direct lighting (the lesson)

An emissive **light tree + NEE/RIS + MIS + ReSTIR DI** path was built and removed. NEE samples a
specific emissive voxel and connects with a **shadow ray** that demands hitting *that* voxel; in a
voxel **cluster** the ray almost always clips a neighbouring emissive voxel first → the sample is
thrown away → fireflies. Area-face sampling, MIS, and ReSTIR DI reduced but never cured it (ReSTIR DI
just turned isolated fireflies into correlated noise). The fix was structural, not a refinement: a
bounce ray finds the *visible* emitter surface with no aiming-and-missing, and this engine already has
a cache to accumulate it. **Lesson (PHILOSOPHIES §1F): let the content class and existing assets gate
the technique — NEE buys small/distant point lights, which this cluster-content engine does not have,
at the cost of catastrophic self-occlusion on the clusters it does have.** Don't re-introduce a
shadow-ray light connection for voxel-cluster emitters.
