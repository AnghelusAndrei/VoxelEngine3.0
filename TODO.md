Done already (for reference): Low-discrepancy sampling(blue noise), ReSTIR DI, GGX VNDF sampling, importance-aware scheduling/voxel dispach by importance, wavefront refactor, NEE with persistent emissive registry, ACES tonemap, firefly clamp, bounded-EMA lBuffer with motion-adaptive SAMPLE_CAP, nBuffer normal refinement, schedule.comp stratification, stale-reset, claim-map dedup, bounce-0 emissive discovery, RIS-weighted NEE.

* ! Adaptive primary / temporal pixel reuse — TAA-style reproject last frame's primary GBuffer; only re-trace primary rays for moving / disoccluded pixels. Primary is ~30% of frame time at high res — most of it is wasted re-tracing identical pixels.
* ! lBuffer caching for internal SVO nodes — populate radiance at coarser octree levels too; sample lookup descends to finest cached level. Better cache hit rate in sparse / distant regions, gives free LoD-aware lighting.
* ! Voxel-space denoiser — spatial filter over the lBuffer hash (not over pixels). Anchor blur in voxel-id space, blend across neighboring voxels with similar normal/material. Aligned with how the data lives, unlike screen-space denoising.
* MIS between BRDF and NEE — kills the double-counting bias when a BRDF bounce happens to hit a light directly. Removes the 1.3–1.5× over-bright issue we deferred.

* Mipmap / LoD-aware SVO traversal — terminate at coarser nodes when ray cone radius exceeds voxel size. Cuts traversal work proportional to distance.
* Stochastic light tree (PBRT-v4 style) — replaces ReSTIR's "uniform candidate proposal" with a proper spatial hierarchy. Only matters once you have hundreds+ of lights; redundant under #2 in small scenes.
* !! ReSTIR GI — extend reservoir reuse to indirect bounces. Big quality jump for global illumination but real implementation cost.

polish / cinematic:

Bloom + HDR post on the resolved image.
Depth of field + motion blur integrated into primary ray generation.
Volumetric fog / scattering along the trace path.


