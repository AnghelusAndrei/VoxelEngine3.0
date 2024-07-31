#version 430 core
out vec4 FragColor;

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;
uniform int lightNum;
uniform int diffuseSamples;
uniform int reflectionSamples;
uniform float skyboxDiffuseIntensity;
uniform float skyboxSpecularIntensity;

layout (std140) uniform CameraUniform {
    vec4 position;
    vec4 cameraPlane, cameraPlaneRight, cameraPlaneUp;
} camera;

struct Material {
    vec4 color;
    float ambient, diffuse, specular, roughness, reflection, shininess;
    bool emissive;
    float intensity;
};

layout (std140) uniform MaterialUniform {
    Material material[127];
};

const uint type_mask = uint(1), count_mask = uint(14), next_mask = uint(4294967280), material_mask = uint(254);
const float inv_127 = 1.0/127.0;
const uint FLT32_MAX = uint(2139095030);
uint octreeLength;

struct Node {
    bool type;
    uint count, next, material, normal;
};

struct leaf_t { uint size; vec3 position;};
struct voxel_t { bool hit; uint id, material; uvec3 position;};
struct ray_t { vec3 origin, direction, inverted_direction;};

Node UnpackNode(uint raw) { return Node(bool(raw & type_mask), (raw & count_mask) >> 1, (raw & next_mask) >> 4, (raw & material_mask) >> 1, (raw >> 8u) & 0xFFFFFFu);}
vec3 UnpackNormal(uint packedNormal) { return vec3(float(int(packedNormal >> 16u & 0xFFu) - 128) * inv_127, float(int(packedNormal >> 8u & 0xFFu) - 128) * inv_127, float(int(packedNormal & 0xFFu) - 128) * inv_127);}
bool inBounds(vec3 v, float n) { return all(lessThanEqual(vec3(0), v) && lessThanEqual(v, vec3(n, n, n)));}
uint locate(uvec3 pos, uint p2) { return (uint(bool(pos.x & p2)) << 2) | (uint(bool(pos.y & p2)) << 1) | uint(bool(pos.z & p2));}

vec2 rand(vec2 co){return vec2(fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453), fract(cos(dot(co, vec2(87.9898, 13.233))) * 33398.5453));}

vec3 randomVectorInCone(vec3 coneDir, float coneAngle, vec2 rand) {
    vec3 w = normalize(coneDir);
    vec3 u = normalize(cross(w, abs(w.x) > 0.1 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 v = cross(w, u);
    
    float cosTheta = mix(cos(coneAngle), 1.0, rand.x);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = rand.y * 6.28318530718; // 2 * pi

    return normalize(sinTheta * cos(phi) * u + sinTheta * sin(phi) * v + cosTheta * w);
}

vec3 sampleSkybox(vec3 dir){
    return dir;
}

vec4 intersect(ray_t r, vec3 box_min, vec3 box_max) {
    vec3 t1 = (box_min - r.origin) * r.inverted_direction;
    vec3 t2 = (box_max - r.origin) * r.inverted_direction;
    vec3 tmin = min(t1, t2), tmax = max(t1, t2);
    float t_enter = max(max(tmin.x, tmin.y), tmin.z);
    float t_exit = min(min(tmax.x, tmax.y), tmax.z);
    if (t_exit < t_enter || t_exit < 0) return vec4(0.0, 0.0, 0.0, -1.0);
    return vec4(r.direction * (t_enter + 0.01) + r.origin, 1.0);
}

vec3 intersect_inside(ray_t r, vec3 box_min, vec3 box_max) {
    vec3 t1 = (box_min - r.origin) * r.inverted_direction;
    vec3 t2 = (box_max - r.origin) * r.inverted_direction;
    vec3 tmin = min(t1, t2), tmax = max(t1, t2);
    float t_exit = min(min(tmax.x, tmax.y), tmax.z);
    return r.direction * (t_exit + 0.01) + r.origin;
}

voxel_t Raycast(ray_t ray) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    voxel_t voxel = voxel_t(false, uint(0), uint(0), uvec3(0,0,0));
    uint offset = uint(0), depth = uint(0), q = uint(0);
    vec3 r_pos;

    ray.origin += ray.direction * 3;
    
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
            if (leaf.material != uint(0)) return voxel_t(true, offset, leaf.material, uvec3(target.position));
        }

        r_pos = intersect_inside(ray, target.position, target.position + vec3(target.size));
    }
    return voxel;
}

struct BRDF_t{
    vec3 ambient, diffuse, specular, reflective;
};

BRDF_t computeBRDF(ray_t sampleRay, voxel_t sampleCast, Material mat, vec3 origin, vec3 normal, vec3 reflection){
    BRDF_t brdf = BRDF_t(vec3(0,0,0), vec3(0,0,0), vec3(0,0,0), vec3(0,0,0));

    float diffuse_dot = dot(sampleRay.direction, normal);
    float specular_dot = dot(sampleRay.direction, reflection);
    if(sampleCast.hit)
    {
        Node sampleData = UnpackNode(texelFetch(octreeTexture, int(sampleCast.id)).r);
        vec3 sampleNormal = normalize(UnpackNormal(sampleData.normal));
        Material sampleMat = material[sampleData.material];

        if(sampleMat.emissive){
            float dist = distance(origin, sampleCast.position);
            brdf.diffuse += sampleMat.color.rgb * diffuse_dot * int(diffuse_dot>=0) * sampleMat.intensity / dist;
            brdf.specular += pow(specular_dot * int(specular_dot>=0), mat.shininess) * sampleMat.intensity / dist;
        }
        else{
            brdf.reflective += material[sampleCast.material].color.xyz;
            //TODO: multiple bounces
        }
    }else{
        brdf.diffuse += sampleSkybox(sampleRay.direction) * diffuse_dot * int(diffuse_dot>=0) * skyboxDiffuseIntensity;
        brdf.specular += sampleSkybox(sampleRay.direction) * pow(specular_dot * int(specular_dot>=0), mat.shininess) * skyboxSpecularIntensity;
        brdf.reflective += sampleSkybox(sampleRay.direction);
    }

    return brdf;
}

void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    voxel_t voxel = Raycast(ray);
    FragColor = vec4(sampleSkybox(ray.direction), 0.0);
    vec3 v_pos = vec3(voxel.position) + vec3(0.5, 0.5, 0.5);
    if(voxel.hit){
        Node data = UnpackNode(texelFetch(octreeTexture, int(voxel.id)).r);
        vec3 normal = normalize(UnpackNormal(data.normal));
        Material mat = material[data.material];

        if(mat.emissive){
            FragColor = vec4(mat.color.xyz, float(voxel.id % uint(256)) / 256.0);
            return;
        }


        BRDF_t diffuse_brdf = BRDF_t(mat.color.xyz, vec3(0,0,0), vec3(0,0,0), vec3(0,0,0));
        BRDF_t reflective_brdf = BRDF_t(mat.color.xyz, vec3(0,0,0), vec3(0,0,0), vec3(0,0,0));
        vec3 reflection = normalize(reflect(v_pos - ray.origin,normal));

        //diffuse BRDF
        for(int i = 0; i < diffuseSamples; i++){
            ray_t sampleRay = ray_t(v_pos, normalize(randomVectorInCone(normal, radians(90.0), rand(vec2(vertexPosition.x + i, vertexPosition.y-i)))), vec3(0,0,0));
            sampleRay.inverted_direction = 1.0/sampleRay.direction;
            voxel_t sampleCast = Raycast(sampleRay);
            BRDF_t sampleBRDF = computeBRDF(sampleRay, sampleCast, mat, v_pos, normal, reflection);
            diffuse_brdf.ambient += sampleBRDF.ambient;
            diffuse_brdf.diffuse += sampleBRDF.diffuse;
            diffuse_brdf.specular += sampleBRDF.specular;
            diffuse_brdf.reflective += sampleBRDF.reflective;
        }

        //reflection BRDF
        for(int i = 0; i < reflectionSamples; i++){
            ray_t sampleRay = ray_t(v_pos, normalize(randomVectorInCone(reflection, radians(min(90.0 * mat.roughness, degrees(90 - acos(dot(normal, reflection))))), rand(vec2(vertexPosition.x + i, vertexPosition.y-i)))), vec3(0,0,0));
            sampleRay.inverted_direction = 1.0/sampleRay.direction;
            voxel_t sampleCast = Raycast(sampleRay);
            BRDF_t sampleBRDF = computeBRDF(sampleRay, sampleCast, mat, v_pos, normal, reflection);
            reflective_brdf.ambient += sampleBRDF.ambient;
            reflective_brdf.diffuse += sampleBRDF.diffuse;
            reflective_brdf.specular += sampleBRDF.specular;
            reflective_brdf.reflective += sampleBRDF.reflective;
        }

        float invDiffuseSamples = 1.0/float(diffuseSamples);
        float invRelfectSamples = 1.0/float(reflectionSamples);

        diffuse_brdf.ambient *= invDiffuseSamples;
        diffuse_brdf.diffuse *= invDiffuseSamples;
        diffuse_brdf.specular *= invDiffuseSamples;
        diffuse_brdf.reflective *= invDiffuseSamples;

        reflective_brdf.ambient *= invRelfectSamples;
        reflective_brdf.diffuse *= invRelfectSamples;
        reflective_brdf.specular *= invRelfectSamples;
        reflective_brdf.reflective *= invRelfectSamples; 

        vec3 color = (diffuse_brdf.ambient + reflective_brdf.ambient) * mat.ambient + 
                (diffuse_brdf.diffuse + reflective_brdf.diffuse) * mat.diffuse +
                (diffuse_brdf.reflective + reflective_brdf.reflective) * mat.reflection + 
                (diffuse_brdf.specular + reflective_brdf.specular) * mat.specular;

        FragColor = vec4(color.xyz, uintBitsToFloat(voxel.id % FLT32_MAX));
    }
}