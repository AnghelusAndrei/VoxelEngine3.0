const uint type_mask = uint(1), count_mask = uint(14), next_mask = uint(4294967280), material_mask = uint(254);
const float inv_127 = 1.0/127.0;
uint octreeLength;

uniform samplerCube skybox;

struct Node {
    bool type;
    uint count, next, material, normal;
};

struct leaf_t { uint size; vec3 position;};
// hit_t.size holds the voxel's lattice extent (1, 2, 4, …). Needed by NEE so
// the emissive list can sample a uniform point on the voxel's surface area.
struct hit_t { bool hit; uint id, material; uvec3 position; uint size;};
struct ray_t { vec3 origin, direction, inverted_direction;};

Node UnpackNode(uint raw) { return Node(bool(raw & type_mask), (raw & count_mask) >> 1, (raw & next_mask) >> 4, (raw & material_mask) >> 1, (raw >> 8u) & 0xFFFFFFu);}
// Decode a 10-10-10 pckd normal (stored in the nBuffer by the accum pass).
// Encoding: each component c stored as uint10 = round((c+1)*511), range [0,1022].
// Decode: float(v)/511.0 - 1.0  →  exactly 0 at v=511, ±1 at v=0/1022.
vec3 UnpackNormal(uint p) {
    return vec3(
        float((p >> 22u) & 0x3FFu) / 511.0 - 1.0,
        float((p >> 12u) & 0x3FFu) / 511.0 - 1.0,
        float((p >>  2u) & 0x3FFu) / 511.0 - 1.0
    );
}
bool inBounds(vec3 v, float n) { return all(lessThanEqual(vec3(0), v) && lessThanEqual(v, vec3(n, n, n)));}
uint locate(uvec3 pos, uint p2) { return (uint(bool(pos.x & p2)) << 2) | (uint(bool(pos.y & p2)) << 1) | uint(bool(pos.z & p2));}
float lerp(float a, float b, float t){ return a + t * (b - a);}
vec3 lerp(vec3 a, vec3 b, float t){ return vec3(lerp(a.x,b.x,t), lerp(a.y,b.y,t), lerp(a.z,b.z,t));}


// ============ Low-Discrepancy (Halton) sampling ==============================
// rand(state) used to be PCG white noise — fast but high-variance. LDS gives
// the same call-site contract (returns float in [0,1), advances state in
// place) but uses the Halton sequence with Owen-style block scrambling so
// successive draws are *correlated in a low-discrepancy way* rather than
// independent. Net effect: variance drops as ~1/N instead of ~1/√N for
// reasonably-low-dimensional integrands. In practice you see noticeably
// faster convergence on diffuse / specular shading and NEE.
//
// State encoding (32-bit):
//   * high 24 bits = scrambled sample index — set once per ray at emit time
//     and walks one step per frame so the same pixel/path traces a coherent
//     LDS sequence over time.
//   * low  8 bits  = per-call dimension counter — advances on each rand()
//     call so successive draws within one shade evaluation use different
//     Halton dimensions (i.e. different prime bases).
//
// We have 16 distinct prime bases. For dim ≥ 16 the dim block index XORs the
// sample index, which is an Owen-scramble-style trick — different dim blocks
// see independent LDS sequences even though they cycle the same base table.
// =============================================================================

const uint HALTON_BASES[16] = uint[](
    2u,  3u,  5u,  7u, 11u, 13u, 17u, 19u,
    23u, 29u, 31u, 37u, 41u, 43u, 47u, 53u
);

// Radical-inverse / Halton element. For sample index `i` and prime `b`,
// returns Σ_k (digit_k(i, b) / b^(k+1)). Loop count is bounded by log_b(i);
// the cap of 24 covers a 24-bit sample index even for b = 2 (worst case).
float halton(uint i, uint b) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 24 && i > 0u; k++) {
        f /= float(b);
        uint q = i / b;
        r += f * float(i - q * b);     // i mod b — division is on uint, exact
        i  = q;
    }
    return r;
}

float rand(inout uint state) {
    uint dim         = state & 0xFFu;
    uint sample_idx  = state >> 8u;
    state            = (sample_idx << 8u) | ((dim + 1u) & 0xFFu);

    uint base_idx    = dim & 15u;
    uint dim_block   = dim >> 4u;          // every 16 dims we cycle bases…
    uint base        = HALTON_BASES[base_idx];
    // …so XOR a block-specific scramble into the sample idx — different
    // dim blocks now see decorrelated Halton sequences instead of repeating.
    uint effective   = sample_idx ^ (dim_block * 0x9E3779B9u);
    return halton(effective, base);
}

// Integer variant for code that only wants a uint hash. Kept on PCG since
// nothing samples-converges through this path (used for bit scrambles, not
// integration). Drops the inout — callers were inconsistent about it anyway.
uint randu(uint state){
    state = state * uint(747796405) + uint(2891336453);
    uint word = ((state >> ((state >> uint(28)) + uint(4))) ^ state) * uint(277803737);
    return (word >> uint(22)) ^ word;
}


float randInNormalDistribution(inout uint state){
    float theta = 2 * 3.1415926 * rand(state);
    float rho = sqrt(-2 * log(rand(state)));
    return rho * cos(theta);
}

vec3 RandomDirection(inout uint state){
    float x = randInNormalDistribution(state);
    float y = randInNormalDistribution(state);
    float z = randInNormalDistribution(state);
    return normalize(vec3(x, y, z));
}

// ============ BRDF FUNCTIONS (Fast Normalized Phong for Low-Variance Convergence) ============
// Faster, lower variance than Cook-Torrance, ideal for differentiable rendering

// Normalized Phong BRDF: much faster convergence than microfacet models
// Maps roughness linearly to Phong exponent: specular_exp = 2 / (roughness^2)
// This provides good perceptual control while maintaining low variance
vec3 EvaluateBRDF(vec3 V, vec3 L, vec3 N, vec3 albedo, float roughness, 
                   float metallic, float specularIntensity) {
    
    float NdotL = max(dot(N, L), 0.001);
    if (NdotL < 0.001) return vec3(0.0);
    
    // Convert roughness [0,1] to Phong exponent [2, 256]
    // Higher roughness = lower exponent = wider specular lobe = faster convergence
    float roughClamped = max(roughness, 0.01);
    float specExp = 2.0 / (roughClamped * roughClamped);
    
    // Compute half-vector for specular
    vec3 H = normalize(L - V);
    float NdotH = max(dot(N, H), 0.001);
    
    // Normalized Phong specular (Blinn variant, more efficient)
    // Formula: (n+2)/(2π) * max(NdotH, 0)^n
    float specNorm = (specExp + 2.0) * 0.15915494;  // 1/(2π)
    float specular = specNorm * pow(NdotH, specExp);
    
    // Energy conservation: specular intensity reduces diffuse
    float kS = specularIntensity * metallic;  // Metallic controls specular strength
    float kD = 1.0 - kS * 0.5;  // Prevent over-darkening
    
    // Lambertian diffuse: 1/π = 0.31831
    vec3 diffuse = albedo * kD * 0.31831;
    vec3 spec = vec3(specular * kS);
    
    return (diffuse + spec) * NdotL;
}

// ============ GGX IMPORTANCE SAMPLING (VNDF — Heitz 2018) ============
// For smooth metals the Phong cosine-weighted sample almost never lands on
// the specular lobe, so variance explodes. Half-vector sampling fixes most
// of that but still wastes draws on microfacets that face away from the
// viewer (back-faces of the lobe). VNDF — sampling from the *visible*
// normal distribution conditioned on V — concentrates draws on the
// microfacets the viewer can actually see, which is exactly where the
// reflection contribution is non-zero. Result: lower variance at grazing
// angles, and the throughput collapses to F · G1(L) — no D, no NdotH
// division, no Jacobian terms (they all cancel in the change of variables).
// Reference: Heitz, "Sampling the GGX Distribution of Visible Normals" (2018).

// Smith G1 under separable Schlick/GGX. Used by the throughput below + by
// any other code that needs the masking term.
float smithG1GGX(float NdotX, float a) {
    float a2 = a * a;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// Sample a half-vector H from the visible-normal distribution of GGX given
// the view direction V. α = roughness² (standard convention). All steps
// happen in the local tangent frame around N; the result is rotated back to
// world space at the end.
//
//   1. Stretch V into the isotropic frame (xy ×= α).
//   2. Build an orthonormal basis around the stretched view.
//   3. Sample a 2-D point on the Vh-aligned hemisphere disk, biased so the
//      density matches the visible-microfacet projection.
//   4. Reproject back onto the hemisphere → sampled normal Nh in stretched
//      space.
//   5. Unstretch (xy ÷= α) to get the half-vector in tangent space.
vec3 sampleGGXVNDF(vec3 N, vec3 V, float roughness, inout uint state) {
    float a = max(roughness * roughness, 0.001);

    // Tangent frame around N (same |N.z|<0.999 guard as before).
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);

    // V in tangent space.
    vec3 Vl = vec3(dot(V, T), dot(V, B), dot(V, N));

    // Stretch.
    vec3 Vh = normalize(vec3(a * Vl.x, a * Vl.y, Vl.z));

    // Build a local frame around Vh.
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3  T1    = lensq > 0.0
                  ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq)
                  : vec3(1.0, 0.0, 0.0);
    vec3  T2    = cross(Vh, T1);

    // Concentric-disk sample biased toward V's hemisphere.
    float r1   = rand(state);
    float r2   = rand(state);
    float r    = sqrt(r1);
    float phi  = 6.2831853 * r2;
    float t1   = r * cos(phi);
    float t2   = r * sin(phi);
    float s    = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;

    // Project to hemisphere, then unstretch.
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    vec3 Hl = normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));

    // Tangent → world.
    return Hl.x * T + Hl.y * B + Hl.z * N;
}

// Throughput for a VNDF-sampled GGX reflection: BRDF·NdotL / pdf(L).
// Derivation: the VNDF pdf(L) = G1(V)·D(H) / (4·NdotV); Walter's BRDF is
// F·G·D / (4·NdotV·NdotL). With separable Smith (G = G1(V)·G1(L)) the D,
// G1(V), and the 4·NdotV·NdotL terms all cancel, leaving:
//
//        BRDF · NdotL / pdf(L)  =  F · G1(L)
//
// This is the big win over half-vector sampling — that variant left a
// VdotH / (NdotV·NdotH) factor behind, which spikes at grazing angles.
//
// V points from surface toward viewer; L is the sampled reflected
// direction; H is the sampled half-vector returned by sampleGGXVNDF.
vec3 ggxThroughput(vec3 N, vec3 V, vec3 L, vec3 H, float roughness, vec3 F0) {
    float NdotL = max(dot(N, L), 0.001);
    float VdotH = max(dot(V, H), 0.001);
    float a     = roughness * roughness;
    vec3  F     = F0 + (vec3(1.0) - F0) * pow(1.0 - VdotH, 5.0);
    return F * smithG1GGX(NdotL, a);
}

vec3 sampleSkybox(vec3 dir){
    return texture(skybox, dir).rgb;
}

vec4 intersect(ray_t r, vec3 box_min, vec3 box_max) {
    vec3 t1 = (box_min - r.origin + 0.001) * r.inverted_direction;
    vec3 t2 = (box_max - r.origin - 0.001) * r.inverted_direction;
    vec3 tmin = min(t1, t2), tmax = max(t1, t2);
    float t_enter = max(max(tmin.x, tmin.y), tmin.z);
    float t_exit = min(min(tmax.x, tmax.y), tmax.z);
    if (t_exit < t_enter || t_exit < 0) return vec4(0.0, 0.0, 0.0, -1.0);
    return vec4(r.direction * (t_enter) + r.origin, 1.0);
}

vec3 intersect_inside(ray_t r, vec3 box_min, vec3 box_max) {
    vec3 t1 = (box_min - r.origin - 0.001) * r.inverted_direction;
    vec3 t2 = (box_max - r.origin + 0.001) * r.inverted_direction;
    vec3 tmin = min(t1, t2), tmax = max(t1, t2);
    float t_exit = min(min(tmax.x, tmax.y), tmax.z);
    return r.direction * (t_exit) + r.origin;
}

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

uint voxelKey(hit_t voxel) {
    // Stable, position-based hash. voxel.id is the SVO linear offset and is NOT
    // stable across scene edits: when the octree is re-linearised, the same
    // spatial voxel ends up at a different offset, which invalidates every
    // nBuffer/lBuffer entry keyed on it (seen as wrong normals drifting in
    // over time). Position is invariant, so keying on it survives rebuilds.
    //
    // Encoding: bit-pack 10 bits per axis (supports octreeDepth <= 10, the same
    // limit already assumed by the packed half-coord MRT). At higher depths the
    // axes overlap but the mix below still yields a well-distributed hash.
    // hash() decorrelates spatially adjacent voxels so nBuffer buckets don't
    // cluster along planes.
    uint p = voxel.position.x
           | (voxel.position.y << 10u)
           | (voxel.position.z << 20u);
    // Mask to 31 bits so (voxelKey + 1) can never wrap to 0 — the miss sentinel
    // used throughout the pipeline. Otherwise 1-in-2^32 voxels would silently
    // fall out of the lBuffer/nBuffer.
    return hash(p) & 0x7FFFFFFFu;
}

// Counted variant of Raycast — identical logic but exposes the traversal
// check counter used by the STRUCTURE visualization. Kept separate so the
// hot path (Raycast) stays an `out`-less function that the compiler can
// inline aggressively.
hit_t RaycastCounted(ray_t ray, out uint checks) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    hit_t voxel = hit_t(false, uint(0), uint(0), uvec3(0,0,0), uint(0));
    uint offset = uint(0), depth = uint(0), q = uint(0);
    vec3 r_pos;

    ray.origin += ray.direction * 4;

    if (inBounds(ray.origin, float(octreeLength))) r_pos = ray.origin;
    else {  vec4 intersection = intersect(ray, vec3(0), vec3(float(octreeLength)));
            r_pos = intersection.xyz; q++;
            if (intersection.w < 0.0) { checks = q; return voxel; }}

    leaf_t target;

    while (inBounds(r_pos, float(octreeLength)) && q++ <= controlchecks) {
        uvec3 ur_pos = uvec3(uint(r_pos.x), uint(r_pos.y), uint(r_pos.z));
        depth = offset = uint(0);
        bool foundLeaf = false;

        for (; depth < octreeDepth; depth++) {
            offset += locate(ur_pos, p2c[depth]);
            Node leaf = UnpackNode(texelFetch(octreeTexture, int(offset)).r);
            if (!leaf.type) {
                target.size = p2c[depth];
                target.position = vec3(uvec3(ur_pos) & ~uvec3(target.size - uint(1)));
                foundLeaf = true;
                break;
            }
            offset = leaf.next;
        }

        if (!foundLeaf) {
            offset += locate(ur_pos, p2c[depth]);
            Node leaf = UnpackNode(texelFetch(octreeTexture, int(offset)).r);
            target.size = p2c[depth];
            target.position = vec3(uvec3(ur_pos) & ~uvec3(target.size - uint(1)));
            uint voxelID = offset;
            if (leaf.material != uint(0)) {
                checks = q;
                return hit_t(true, voxelID, leaf.material, uvec3(target.position), target.size);
            }
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    checks = q;
    return voxel;
}

hit_t Raycast(ray_t ray) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    hit_t voxel = hit_t(false, uint(0), uint(0), uvec3(0,0,0), uint(0));
    uint offset = uint(0), depth = uint(0), q = uint(0);
    vec3 r_pos;

    ray.origin += ray.direction * 4;

    if (inBounds(ray.origin, float(octreeLength))) r_pos = ray.origin;
    else {  vec4 intersection = intersect(ray, vec3(0), vec3(float(octreeLength)));
            r_pos = intersection.xyz; q++;
            if (intersection.w < 0.0) return voxel;}

    leaf_t target;

    while (inBounds(r_pos, float(octreeLength)) && q++ <= controlchecks) {
        uvec3 ur_pos = uvec3(uint(r_pos.x), uint(r_pos.y), uint(r_pos.z));
        depth = offset = uint(0);
        bool foundLeaf = false;

        for (; depth < octreeDepth; depth++) {
            offset += locate(ur_pos, p2c[depth]);
            Node leaf = UnpackNode(texelFetch(octreeTexture, int(offset)).r);
            if (!leaf.type) {
                target.size = p2c[depth];
                target.position = vec3(uvec3(ur_pos) & ~uvec3(target.size - uint(1)));
                foundLeaf = true;
                break;
            }
            offset = leaf.next;
        }

        if (!foundLeaf) {
            offset += locate(ur_pos, p2c[depth]);
            Node leaf = UnpackNode(texelFetch(octreeTexture, int(offset)).r);
            target.size = p2c[depth];
            target.position = vec3(uvec3(ur_pos) & ~uvec3(target.size - uint(1)));
            uint voxelID = offset;
            //uint voxelID = uint(target.position.x) + uint(target.position.y) * octreeLength + uint(target.position.z) * octreeLength * octreeLength;
            if (leaf.material != uint(0)) return hit_t(true, voxelID, leaf.material, uvec3(target.position), target.size);
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    return voxel;
}

// Shadow-ray helper for NEE. Returns true iff the first voxel hit by `ray`
// matches `target_vid` (== voxelKey(hit)+1). Accounts for self-occlusion: the
// caller is expected to start the ray slightly off the surface so the
// shading point's own voxel doesn't shadow itself.
bool RaycastVisibilityToVoxel(ray_t ray, uint target_vid) {
    hit_t hit = Raycast(ray);
    if (!hit.hit) return false;
    return (voxelKey(hit) + 1u) == target_vid;
}