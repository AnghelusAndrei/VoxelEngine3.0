// Shared NEE / ReSTIR target. Included by BOTH restir.comp (builds the reservoir) and shade.comp
// (temporal read + spatial Z-combine). MUST be one definition: restir's W = wSum/(M·p̂) and shade's
// f·W cancellation both depend on p̂ being byte-identical across the two passes — a divergence is the
// geometry-mismatch class of bug (see the M≥2 firefly regression). architecture/RESTIR.md.
//
// Requires (declared by the includer, before this include): the `Material material[]` UBO and
// `dominantFaceNormal` (from lighttree.glsl). Include AFTER both.

// Unshadowed RIS target p̂(y): luminance contribution of the light at `lightPos` to the voxel at
// `center` / normal `N`, evaluated centre-based + dominant-face (deterministic given the light, so
// the same value is recovered on reuse). Any positive function is a valid RIS target; this one
// importance-samples near/bright lights. Returns 0 if the light is below the horizon (no contribution).
float targetPhat(vec3 center, vec3 N, uvec3 lightPos, uint lightSize, uint lightMat){
    vec3  lc  = vec3(lightPos) + float(lightSize) * 0.5;
    vec3  toL = lc - center;
    float d2  = max(dot(toL, toL), 1e-8);
    vec3  wL  = toL * inversesqrt(d2);
    float cosSurf  = max(dot(N, wL), 0.0);
    float cosLight = max(dot(dominantFaceNormal(-toL), -wL), 0.0);
    if (cosSurf <= 0.0 || cosLight <= 0.0) return 0.0;
    Material lm = material[lightMat];
    float lum = dot(lm.color.rgb, vec3(0.2126, 0.7152, 0.0722)) * lm.emissiveIntensity;
    float A   = float(lightSize) * float(lightSize);
    return lum * A * cosSurf * cosLight / (3.14159265 * d2);
}
