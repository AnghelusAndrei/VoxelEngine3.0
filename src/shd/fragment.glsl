#version 330 core
out vec4 FragColor;

in vec4 vertexPosition;

uniform usamplerBuffer octreeTexture;
uniform uint octreeDepth;
uniform int lightNum;
uniform int lightSamples;
uniform int reflectionSamples;

layout (std140) uniform CameraUniform {
    vec4 position;
    vec4 cameraPlane, cameraPlaneRight, cameraPlaneUp;
} camera;

struct Light{
    vec4 position, color;
    float radius, intensity, area;
};

layout (std140) uniform LightUniform {
    Light lights[256];
};

struct Material {
    vec4 color;
    float ambient, diffuse, specular, roughness, reflection;
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
struct voxel_t { bool hit; uint id, material; uvec3 position;};
struct ray_t { vec3 origin, direction, inverted_direction;};

Node UnpackNode(uint raw) { return Node(bool(raw & type_mask), (raw & count_mask) >> 1, (raw & next_mask) >> 4, (raw & material_mask) >> 1, (raw >> 8u) & 0xFFFFFFu);}
vec3 UnpackNormal(uint packedNormal) { return vec3(float(int(packedNormal >> 16u & 0xFFu) - 128) * inv_127, float(int(packedNormal >> 8u & 0xFFu) - 128) * inv_127, float(int(packedNormal & 0xFFu) - 128) * inv_127);}
bool inBounds(vec3 v, float n) { return all(lessThanEqual(vec3(0), v) && lessThanEqual(v, vec3(n, n, n)));}
uint locate(uvec3 pos, uint p2) { return (uint(bool(pos.x & p2)) << 2) | (uint(bool(pos.y & p2)) << 1) | uint(bool(pos.z & p2));}

vec2 rand(vec2 co){return vec2(fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453), fract(cos(dot(co, vec2(87.9898, 13.233))) * 33398.5453));}

vec3 randomVectorInConeTangentToSphere(vec3 coneApex, vec3 sphereCenter, float sphereRadius, vec2 rand) {
    vec3 coneDir = normalize(sphereCenter - coneApex);
    float coneAngle = asin(sphereRadius / length(sphereCenter - coneApex));

    vec3 w = normalize(coneDir);
    vec3 u = normalize(cross(w, abs(w.x) > 0.1 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 v = cross(w, u);
    
    float cosTheta = mix(cos(coneAngle), 1.0, rand.x);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = rand.y * 6.28318530718; // 2 * pi

    return normalize(sinTheta * cos(phi) * u + sinTheta * sin(phi) * v + cosTheta * w);
}

vec3 randomVectorInCone(vec3 coneDir, float coneAngle, vec2 rand) {
    vec3 w = normalize(coneDir);
    vec3 u = normalize(cross(w, abs(w.x) > 0.1 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 v = cross(w, u);
    
    float cosTheta = mix(cos(coneAngle), 1.0, rand.x);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = rand.y * 6.28318530718; // 2 * pi

    return normalize(sinTheta * cos(phi) * u + sinTheta * sin(phi) * v + cosTheta * w);
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

voxel_t Raycast(ray_t ray, float l) {
    uint p2c[16];
    for (int i = 0; i <= int(octreeDepth); i++) {
        p2c[i] = uint(octreeLength >> uint(i));
    }

    voxel_t voxel = voxel_t(false, uint(0), uint(0), uvec3(0,0,0));
    uint offset = uint(0), depth = uint(0), q = uint(0);
    vec3 r_pos;
    
    if (inBounds(ray.origin, float(octreeLength))) r_pos = ray.origin;
    else {  vec4 intersection = intersect(ray, vec3(0), vec3(float(octreeLength)));
            r_pos = intersection.xyz; q++;
            if (intersection.w < 0.0 || distance(ray.origin, r_pos) > l) return voxel;}

    leaf_t target;

    while (inBounds(r_pos, float(octreeLength)) && q++ <= uint(65) && distance(ray.origin, r_pos) < l) {
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

        if (!foundLeaf && q > uint(1)) {
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

void main() {
    vec3 direction = normalize(camera.cameraPlane.xyz + vertexPosition.x * camera.cameraPlaneRight.xyz - vertexPosition.y * camera.cameraPlaneUp.xyz);
    octreeLength = uint(1) << octreeDepth;

    ray_t ray;
    ray.origin = camera.position.xyz;
    ray.direction = direction;
    ray.inverted_direction = 1.0 / direction;

    voxel_t voxel = Raycast(ray, 1e9);
    FragColor = vec4(direction, 0.0);
    if(voxel.hit){
        Node data = UnpackNode(texelFetch(octreeTexture, int(voxel.id)).r);
        vec3 normal = normalize(UnpackNormal(data.normal));
        Material mat = material[data.material];
        vec3 color = mat.color.xyz * mat.ambient;

        vec3 luminance = vec3(0,0,0); int affected = 0;
        for(int i = 0; i < lightNum; i++){
            float light_dist = distance(lights[i].position.xyz, vec3(voxel.position)) - lights[i].radius;
            if(light_dist > lights[i].area) continue;
            vec3 light_vec = normalize(lights[i].position.xyz - vec3(voxel.position));
            int hits = Raycast(ray_t(vec3(voxel.position) + vec3(0.5, 0.5, 0.5), light_vec, 1.0/light_vec), light_dist).hit ? 1 : 0;
            for(int i = 1; i < lightSamples; i++){
                vec3 light_vec = normalize(randomVectorInConeTangentToSphere(vec3(voxel.position) + vec3(0.5, 0.5, 0.5), lights[i].position.xyz, lights[i].radius, rand(vec2(vertexPosition.x + i, vertexPosition.y-i))));
                vec3 inverse_vec = 1.0/light_vec;
                ray_t light_ray = ray_t(vec3(voxel.position) + vec3(0.5, 0.5, 0.5), light_vec, inverse_vec);
                voxel_t light_detection = Raycast(light_ray, light_dist);
                if(!light_detection.hit)hits++;
            }

            if(hits>0){
                affected++;
                vec3 viewReflection = reflect(normalize(vec3(voxel.position) - ray.origin), normal);
                float diffuse_dot = dot(light_vec, normal);
                float specular_dot = dot(light_vec, viewReflection);
                luminance += (mat.color.xyz * diffuse_dot * int(diffuse_dot>=0) * mat.diffuse +
                        pow(specular_dot * int(specular_dot>=0), mat.specular)) * 
                        lights[i].area/light_dist * lights[i].intensity * lights[i].color.xyz * float(hits)/float(lightSamples);
            }
        }

        vec3 absorbed = vec3(0,0,0);
        vec3 reflection = normalize(reflect(vec3(voxel.position) + vec3(0.5, 0.5, 0.5) - ray.origin,normal));
        for(int i = 0; i < reflectionSamples; i++){
            vec3 vec = normalize(randomVectorInCone(reflection, radians(180.0 * mat.roughness), rand(vec2(vertexPosition.x + i, vertexPosition.y-i))));
            vec3 inverse_vec = 1.0/vec;
            ray_t reflection_ray = ray_t(vec3(voxel.position) + vec3(0.5, 0.5, 0.5), vec, inverse_vec);
            voxel_t reflection_detection = Raycast(reflection_ray, 1e9);
            absorbed += reflection_detection.hit ? material[reflection_detection.material].color.xyz : vec;
        }
        absorbed = absorbed / reflectionSamples;
        color+=luminance/float(affected == 0 ? 1 : affected);
        color = color * 1.0/mat.reflection + mat.reflection * absorbed;
        FragColor = vec4(color.xyz, float(voxel.id % uint(256)) / 256.0);
    }
}