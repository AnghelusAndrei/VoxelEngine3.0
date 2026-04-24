#version 430 core
layout(location = 0) out uvec4 FragColor;    // RGB = lighting, A = voxelKey(voxel)+1 (stable lBuffer/nBuffer key)
layout(location = 1) out uint NormalIdx;    // pckd half-coords for accum neighbourhood lookup

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;

// nBuffer — normal hash map, read-only in the ray pass (sampler for texture cache benefit).
// Stride=2 per slot: s*2=voxelID owner, s*2+1=packed 10-10-10 normal.
// Written by the accum pass (imageAtomicCompSwap); read here via texelFetch.
// GL_TEXTURE_FETCH_BARRIER_BIT after accum ensures coherency between domains.
uniform usampler2D nBuffer;
uniform int nBufferWidth;
uniform int nBufferSlots;
uniform int spp;
uniform uint controlchecks;
uniform int lightBounces;
uniform ivec2 screenResolution;
uniform int time;

#define nBufferStride 3

#ifndef FLT_MAX
#define FLT_MAX 3.402823466e+38
#endif

layout (std140) uniform CameraUniform {
    vec4 position;
    vec4 cameraPlane, cameraPlaneRight, cameraPlaneUp;
} camera;

struct Material {
    vec4 color, specularColor;
    float roughness, specular, metallic;
    bool emissive;
    float emissiveIntensity;
};

layout (std140) uniform MaterialUniform {
    Material material[127];
};

#include "internal.glsl"

vec3 Trace(ray_t ray, hit_t voxel, inout uint randomState){
    vec3 incomingLight = vec3(0,0,0);
    vec3 rayColor = vec3(1,1,1);
    
    for(int i = 0; i <= lightBounces; i++){
        uint raw = texelFetch(octreeTexture, int(voxel.id)).r;
        Node data = UnpackNode(raw);

        // nBuffer lookup: find cached refined normal for this voxel
        uint nKey  = voxelKey(voxel) + 1u;
        uint nHash = nKey % uint(nBufferWidth - 1) + 1u;
        uint normalField = 0u;
        for (int s = 0; s < nBufferSlots; s++) {
            uint owner = texelFetch(nBuffer, ivec2(int(nHash), s * nBufferStride), 0).r;
            if (owner == nKey) {
                normalField = texelFetch(nBuffer, ivec2(int(nHash), s * nBufferStride + 1), 0).r;
                break;
            }
        }

        vec3 normal;
        if (normalField != 0u) {
            normal = normalize(UnpackNormal(normalField));
        } else {
            // Face normal fallback
            vec3 adir = abs(ray.direction);
            if (adir.x > adir.y && adir.x > adir.z)
                normal = vec3(-sign(ray.direction.x), 0.0, 0.0);
            else if (adir.y > adir.z)
                normal = vec3(0.0, -sign(ray.direction.y), 0.0);
            else
                normal = vec3(0.0, 0.0, -sign(ray.direction.z));
        }

        Material mat = material[data.material];
        
        // Generate cosine-weighted importance sample
        vec3 sampleDir = normalize(normal + RandomDirection(randomState));
        
        // Evaluate BRDF: this replaces the old binary specular/diffuse choice
        // EvaluateBRDF handles proper energy conservation and material blending
        vec3 brdfValue = EvaluateBRDF(ray.direction, sampleDir, normal, 
                                      mat.color.xyz, mat.roughness, mat.metallic, mat.specular);
        
        // Update ray for next bounce
        ray.origin = vec3(voxel.position) + vec3(0.5, 0.5, 0.5) + normal;
        ray.direction = sampleDir;
        ray.inverted_direction = 1.0 / ray.direction;

        // Handle emissive materials
        if(mat.emissive){
            incomingLight += mat.color.xyz * mat.emissiveIntensity * rayColor;
        }
        
        // Accumulate BRDF contribution (already includes NdotL weighting)
        rayColor *= brdfValue;

        if(i < lightBounces){
            voxel = Raycast(ray);
            if(!voxel.hit){
                incomingLight += sampleSkybox(ray.direction) * rayColor;
                break;
            }
        }
    }
    return incomingLight;
}

void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    hit_t voxel = Raycast(ray);

    if(voxel.hit){
        vec3 incomingLight = vec3(0,0,0);
        uint px = uint((vertexPosition.x + 1.0) * 0.5 * float(screenResolution.x));
        uint py = uint((vertexPosition.y + 1.0) * 0.5 * float(screenResolution.y));
        uint randomState = (px * 1973u + py * 9277u + uint(time) * 26699u) | 1u;
        for(int i = 0; i < spp; i++){
            incomingLight += Trace(ray, voxel, randomState);
        }
        incomingLight /= float(spp);
        // Alpha = stable position hash — used as the lBuffer and nBuffer key.
        uint hashID = voxelKey(voxel); // *
        FragColor = uvec4(uvec3(incomingLight * 255.0), hashID + 1u);
        // Second attachment: pckd half-coords for accum neighbourhood lookup.
        // Encoding: (x>>1) | ((y>>1)<<bits) | ((z>>1)<<(2*bits)), bits=octreeDepth-1.
        // Scales up to depth=11 (each half-coord needs depth-1 bits, 3*(depth-1)<=30).
        uint bits = octreeDepth - 1u;
        NormalIdx = (voxel.position.x >> 1u)
                  | ((voxel.position.y >> 1u) << bits)
                  | ((voxel.position.z >> 1u) << (2u * bits));
    }else{
        FragColor = uvec4(uvec3(sampleSkybox(ray.direction) * 255.0), 0u);
        NormalIdx = 0u;
    }
}