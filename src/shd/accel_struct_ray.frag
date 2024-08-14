#version 430 core
out vec4 FragColor;

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;
uniform int spp;
uniform int lightBounces;
uniform ivec2 screenResolution;
uniform int time;

#ifndef FLT_MAX
#define FLT_MAX 3.402823466e+38
#endif

layout (std140) uniform CameraUniform {
    vec4 position;
    vec4 cameraPlane, cameraPlaneRight, cameraPlaneUp;
} camera;

struct Material {
    vec4 color, specularColor;
    float diffuse, specular, metallic;
    bool emissive;
    float emissiveIntensity;
};

layout (std140) uniform MaterialUniform {
    Material material[127];
};

const uint type_mask = uint(1), count_mask = uint(14), next_mask = uint(4294967280), material_mask = uint(254);
const float inv_127 = 1.0/127.0;
uint octreeLength;

struct Node {
    bool type;
    uint count, next, material, normal;
};

struct leaf_t { uint size; vec3 position;};
struct hit_t { bool hit; uint id, material; uvec3 position; vec3 q;};
struct ray_t { vec3 origin, direction, inverted_direction;};

Node UnpackNode(uint raw) { return Node(bool(raw & type_mask), (raw & count_mask) >> 1, (raw & next_mask) >> 4, (raw & material_mask) >> 1, (raw >> 8u) & 0xFFFFFFu);}
vec3 UnpackNormal(uint packedNormal) { return vec3(float(int(packedNormal >> 16u & 0xFFu) - 128) * inv_127, float(int(packedNormal >> 8u & 0xFFu) - 128) * inv_127, float(int(packedNormal & 0xFFu) - 128) * inv_127);}
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

vec3 sampleSkybox(vec3 dir){
    return dir;
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

hit_t Raycast(ray_t ray) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    hit_t voxel = hit_t(false, uint(0), uint(0), uvec3(0,0,0), vec3(0,0,0));
    uint offset = uint(0), depth = uint(0), q = uint(0);
    vec3 r_pos;

    ray.origin += ray.direction * 4;
    
    if (inBounds(ray.origin, float(octreeLength))) r_pos = ray.origin;
    else {  vec4 intersection = intersect(ray, vec3(0), vec3(float(octreeLength)));
            r_pos = intersection.xyz; q++;
            if (intersection.w < 0.0) return voxel;}

    leaf_t target;

    while (inBounds(r_pos, float(octreeLength)) && q++ <= uint(65)) {
        uvec3 ur_pos = uvec3(uint(r_pos.x), uint(r_pos.y), uint(r_pos.z));
        depth = offset = uint(0);
        bool foundLeaf = false;

        for (; depth < octreeDepth - uint(1); depth++) {
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
            if (leaf.material != uint(0)) return hit_t(true, offset, leaf.material, uvec3(target.position), vec3(float(q),float(q),float(q)));
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    voxel.q = vec3(float(q),float(q),float(q));
    return voxel;
}


void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    hit_t voxel = Raycast(ray);

    FragColor = vec4(voxel.q / 65.0, 0);
}