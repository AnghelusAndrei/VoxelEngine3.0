# ReSTIR GI — Per-Voxel Bounce-Sample Reservoir

> **Status: design (next to build).** The current renderer ([PIPELINE.md](PIPELINE.md)) is a per-voxel
> irradiance-cache path tracer: direct light is the diffuse **bounce** landing on an emissive voxel, and
> the per-voxel cache + temporal EMA accumulate it. ReSTIR GI adds **spatiotemporal reservoir reuse of
> the bounce samples** so convergence is fast for the cases temporal-alone is slow on (disocclusion,
> rare bounce hits on small/occluded emitters). It denoises **direct emission AND one-bounce indirect**
> with a single reservoir, and uses **no NEE / no light list / no shadow-ray connection** — so the
> firefly-via-self-occlusion class (why the [ROADMAP.md](ROADMAP.md) "Rejected" NEE path was removed) is
> gone by construction.

## The one idea

A ReSTIR GI **sample** is: *the bounce ray I shot, the voxel it hit, and how bright that voxel is looking
back at me.* The brightness depends only on what the bounce hit:

- bounce hit an **emissive voxel** → brightness = its emission ⇒ **direct light**;
- bounce hit a **lit surface** → brightness = its cached radiance (`albedo · cached E/π`) ⇒ **indirect**.

It is the *same sample type* either way, so one reservoir denoises both — the emitter is simply the
*brightest* thing a bounce can land on, and the reservoir's importance sampling prioritises exactly
those. The "verified working path" the bounce gives — visibility is built in, the hit is the first
solid surface, no shadow ray to self-occlude — is precisely the sampling ReSTIR GI reuses.

## Reservoir (reference-based, in-slot)

Store a **reference**, not a payload — the world is voxels, so the sample's radiance already lives in the
hit voxel's own cache slot:

- sample `y` = the bounce-hit voxel → its **lBuffer key** (re-fetch `cache(y).diffuse` + `emission(y)`
  and `cache(y).normal` from `y`'s slot at reuse — both view-independent for diffuse, so valid to reuse);
- `W` (fp32) = the unbiased contribution weight; `M` = confidence count, capped.

This fits the free slot DWORDs **D14/D15** ([LBUFFER.md](LBUFFER.md)); the sample *direction* is implicit
(`normalize(center(y) − center(q))`). Quantizing the sample to voxel granularity is exact — the world is
voxels. The spatiotemporal-feedback machinery built for (and removed with) ReSTIR DI is the reusable
pattern here: a per-voxel pass writing a parallel reservoir that folds into the next frame's temporal
reservoir, **lifecycle-zeroed in `initSlot`** on claim/evict (a reused slot must not serve a prior
voxel's sample).

## The one new cost — the reconnection Jacobian

ReSTIR DI re-evaluated a light from scratch, so it needed no Jacobian. GI **transplants** a sample
between viewpoints: when voxel `q` reuses neighbour `n`'s bounce hit `x`, `q` sees `x` from a different
angle and distance than `n` did, so the borrowed radiance must be rescaled by a geometry factor
(≈ the ratio of `cosθ / distance²` between the two viewpoints), computed from the two voxel centres.
This is the place GI bugs hide — get the Jacobian and the reuse domains right (Bitterli/GRIS), and
verify the converged **mean is unchanged** when reuse is toggled (only noise/convergence may change).

## Visibility on reuse (degrades gracefully)

`q`'s **own** bounce sample needs no connection ray — the bounce already reached `x` and it is the first
hit, so it is visible by definition. That clean self-visible bounce is the noise *floor* — the same
floor as the current best cache+bounce renderer, so GI can never regress below it. Reuse of a neighbour's
sample may reconnect `q → x`; if that fails (occluded), the fallback is `q`'s own sample, so a failed
reuse means *less denoising that frame, not a firefly.* A first version may skip the reconnection
visibility test entirely (slightly biased, clean); add it later if light-leak shows.

## Scope — diffuse + wide-glossy only

GI reuse is gated by roughness. A near-mirror lobe is too tight for a neighbour's sample to be valid, so
**low-roughness / mirror specular stays on the current direct GGX-VNDF trace path** (the specular channel
+ short EMA + à-trous). As roughness rises, neighbour samples become valid and reuse pays off. No hybrid
per-pixel system: sharp specular is still per-voxel traced, just not resampled.

## Curvature carries over

Spatial neighbour selection on a **curved** surface cannot use a flat tangent-plane offset (it leaves the
surface at radius — the lesson from the removed DI spatial pass). Use surface-aware neighbour sampling
(e.g. tangent offset snapped back onto the surface along ±N). Validate neighbour hit-rate before trusting
sphere results.
