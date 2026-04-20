#version 430 core
out vec4 FragColor;

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;
uniform int spp;
uniform uint controlchecks;
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

#include "internal.glsl"


void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    hit_t voxel = Raycast(ray);

    if(voxel.hit){
        Node data = UnpackNode(texelFetch(octreeTexture, int(voxel.id)).r);
        vec3 normal = normalize(UnpackNormal(data.normal));
        Material mat = material[data.material];
        FragColor = vec4(mat.color.xyz, float(voxel.id+1));
    }else{
        FragColor = vec4(sampleSkybox(ray.direction), 0);
    }
}
