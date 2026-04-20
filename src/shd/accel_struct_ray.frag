#version 430 core
layout(location = 0) out vec4 FragColor;
layout(location = 1) out uint NormalIdx;    // packed half-coords (keeps accum pipeline valid)

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;

// nBuffer — same hash map as in ray.frag, read-only.
// Stride=2: s*2=owner, s*2+1=packed 10-10-10 normal.
uniform usampler2D nBuffer;
uniform int nBufferWidth;
uniform int nBufferSlots;

uniform uint controlchecks;
uniform ivec2 screenResolution;
uniform int time;
uniform int spp;
uniform int lightBounces;

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
layout (std140) uniform MaterialUniform { Material material[127]; };

#include "internal.glsl"

hit_t RaycastDebug(ray_t ray, inout uint q) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    hit_t voxel = hit_t(false, uint(0), uint(0), uvec3(0,0,0));
    uint offset = uint(0), depth = uint(0);
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
            uint voxelID = offset;
            if (leaf.material != uint(0)) return hit_t(true, voxelID, leaf.material, uvec3(target.position));
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    return voxel;
}

void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    uint q = uint(0);
    hit_t voxel = RaycastDebug(ray, q);

    if (voxel.hit) {
        // nBuffer lookup — identical to ray.frag.
        uint nKey  = voxel.id + 1u;
        uint nHash = nKey % uint(nBufferWidth - 1) + 1u;
        uint normalField = 0u;
        bool cached = false;
        for (int s = 0; s < nBufferSlots; s++) {
            uint owner = texelFetch(nBuffer, ivec2(int(nHash), s * 2), 0).r;
            if (owner == nKey) {
                normalField = texelFetch(nBuffer, ivec2(int(nHash), s * 2 + 1), 0).r;
                cached = true;
                break;
            }
        }

        vec3 normal;
        if (cached && normalField != 0u) {
            normal = normalize(UnpackNormal(normalField));
        } else {
            // Face normal fallback — same placeholder as ray.frag.
            vec3 adir = abs(direction);
            if (adir.x > adir.y && adir.x > adir.z)
                normal = vec3(-sign(direction.x), 0.0, 0.0);
            else if (adir.y > adir.z)
                normal = vec3(0.0, -sign(direction.y), 0.0);
            else
                normal = vec3(0.0, 0.0, -sign(direction.z));
        }

        vec3 col;
        col.xyz = vec3(float(q)/65.0);
        FragColor = vec4(col.xyz, float(voxel.id + 1u));

        // Keep the packed position output so the accum pipeline stays valid in this mode.
        uint bits = octreeDepth - 1u;
        NormalIdx = (voxel.position.x >> 1u)
                  | ((voxel.position.y >> 1u) << bits)
                  | ((voxel.position.z >> 1u) << (2u * bits));
    } else {
        vec3 col;
        col.xyz = vec3(float(q)/65.0);
        FragColor = vec4(col.xyz, 0.0);
        NormalIdx = 0u;
    }
}
