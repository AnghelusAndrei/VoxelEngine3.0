Done already (for reference): Low-discrepancy sampling(blue noise), ReSTIR DI, ReSTIR GI, GGX VNDF sampling, importance-aware scheduling/voxel dispach by importance, wavefront refactor, NEE with persistent emissive registry, ACES tonemap, firefly clamp, bounded-EMA lBuffer with motion-adaptive SAMPLE_CAP, nBuffer normal refinement, schedule.comp stratification, stale-reset, claim-map dedup, bounce-0 emissive discovery, RIS-weighted NEE.

* ! Adaptive primary / temporal pixel reuse — TAA-style reproject last frame's primary GBuffer; only re-trace primary rays for moving / disoccluded pixels. Primary is ~30% of frame time at high res — most of it is wasted re-tracing identical pixels.
* ! lBuffer caching for internal SVO nodes — populate radiance at coarser octree levels too; sample lookup descends to finest cached level. Better cache hit rate in sparse / distant regions, gives free LoD-aware lighting.
* ! Voxel-space denoiser — spatial filter over the lBuffer hash. 2 ideas:
  - Anisotropic Diffusion (Edge-Preserving PDE Filtering)/Cross-Bilateral Filtering Done Properly (Not A-Trous)/Wavelet / Multigrid Denoising in Voxel Space(might add this anyway with lBuffer caching internal nodes)/Variance-Guided Laplacian Smoothing (Graph/PDE Hybrid)
  - tiny MLP model voxel-space denoiser(my favourite) - get converged/initial frames and runtime and use them in a separate python trainer, probably the cleanest idea
* MIS between BRDF and NEE — kills the double-counting bias when a BRDF bounce happens to hit a light directly. Removes the 1.3–1.5× over-bright issue we deferred. - questionable use might not implement
* Mipmap / LoD-aware SVO traversal — terminate at coarser nodes when ray cone radius exceeds voxel size. Cuts traversal work proportional to distance.
* Stochastic light tree (PBRT-v4 style) — replaces ReSTIR's "uniform candidate proposal" with a proper spatial hierarchy. Only matters once you have hundreds+ of lights; redundant under #2 in small scenes.

polish / cinematic:

Bloom + HDR post on the resolved image.
Depth of field + motion blur integrated into primary ray generation.
Volumetric fog / scattering along the trace path.

* !!! Full scene mem redesign(LRU cache with disk mem, dynamic generation, LOD, Remove OctreeCPU) - The biggest
* marching cubes view mode
* volumetric view mode
* export renders
* import/voxelise 3D files
* module(python) build + proper API
