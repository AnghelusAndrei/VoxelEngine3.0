#version 430 core
layout(location = 0) out vec4 FragColor;
layout(location = 1) out uint NormalIdx;    // packed half-coords (keeps accum pipeline valid)

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;

// nBuffer — same hash map as in ray.frag, read-only.
#define nBufferStride 3 // Stride=3: s*3=owner, s*3+1=packed 10-10-10 normal, s*3+2=timestamp.
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


void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    hit_t voxel = Raycast(ray);

    if (voxel.hit) {
        // nBuffer lookup — identical to ray.frag.
        uint nKey  = voxel.id + 1u;
        uint nHash = nKey % uint(nBufferWidth - 1) + 1u;
        uint normalField = 0u;
        bool cached = false;
        for (int s = 0; s < nBufferSlots; s++) {
            uint owner = texelFetch(nBuffer, ivec2(int(nHash), s * nBufferStride), 0).r;
            if (owner == nKey) {
                normalField = texelFetch(nBuffer, ivec2(int(nHash), s * nBufferStride + 1), 0).r;
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
        col.xyz = vec3(float((voxel.id + 1u) % 255) / 255.0); // Visualise: full-brightness normal map colour for cached normals, dimmed (×0.35) for face-normal placeholders so pending voxels are obvious.
        FragColor = vec4(col.xyz, float(voxel.id + 1u));

        // Keep the packed position output so the accum pipeline stays valid in this mode.
        uint bits = octreeDepth - 1u;
        NormalIdx = (voxel.position.x >> 1u)
                  | ((voxel.position.y >> 1u) << bits)
                  | ((voxel.position.z >> 1u) << (2u * bits));
    } else {
        FragColor = vec4(direction * 0.5 + 0.5, 0.0);
        NormalIdx = 0u;
    }
}
