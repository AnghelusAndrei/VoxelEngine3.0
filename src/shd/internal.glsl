const uint type_mask = uint(1), count_mask = uint(14), next_mask = uint(4294967280), material_mask = uint(254);
const float inv_127 = 1.0/127.0;
uint octreeLength;

uniform samplerCube skybox;

struct Node {
    bool type;
    uint count, next, material, normal;
};

struct leaf_t { uint size; vec3 position;};
struct hit_t { bool hit; uint id, material; uvec3 position;};
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


float rand(inout uint state){
    state = state * uint(747796405) + uint(2891336453);
    uint word = ((state >> ((state >> uint(28)) + uint(4))) ^ state) * uint(277803737);
    word = (word >> uint(22)) ^ word;
    return float(word) / 4294967295.0;
}

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

hit_t Raycast(ray_t ray) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    hit_t voxel = hit_t(false, uint(0), uint(0), uvec3(0,0,0));
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
            if (leaf.material != uint(0)) return hit_t(true, voxelID, leaf.material, uvec3(target.position));
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    return voxel;
}