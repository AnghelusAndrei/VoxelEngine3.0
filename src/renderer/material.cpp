#include "material.hpp"
#include "core.hpp"

static_assert(sizeof(Material) == 64,
    "Material must be exactly 64 bytes to match GLSL std140 array stride");

MaterialPool::MaterialPool() : length(1){
    capacity = MATERIAL_MAX;
    materials.resize(1);   // index 0 = empty sentinel (matches `length` starting at 1)
}

void MaterialPool::GenUBO(){
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferData(GL_SHADER_STORAGE_BUFFER, capacity * (GLsizeiptr)sizeof(Material),
                 NULL, GL_STATIC_DRAW);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_MATERIAL_BINDING, gl_ID);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void MaterialPool::freeVRAM(){
    glDeleteBuffers(1, &gl_ID);
}

MaterialPool::~MaterialPool(){
    freeVRAM();
}

void MaterialPool::sync(){
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(length * sizeof(Material)), materials.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

uint32_t MaterialPool::addMaterial(Material *material, const std::string& name){
    if (length >= capacity) return 0;       // full: caller should collapse() and retry
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                    (GLintptr)(length * sizeof(Material)),
                    (GLsizeiptr)sizeof(Material),
                    material);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (materials.size() <= length) materials.resize(length + 1);
    if (names.size()     <= length) names.resize(length + 1);
    materials[length] = *material;          // mirror on the CPU
    names[length]     = uniqueName(name.empty() ? ("mat" + std::to_string(length)) : name);
    length++;
    return length-1;
}


bool MaterialPool::setMaterial(Material *material, uint32_t index){
    if(index == 0 || index >= length)
        return false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                    (GLintptr)(index * sizeof(Material)),
                    (GLsizeiptr)sizeof(Material),
                    material);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (materials.size() <= index) materials.resize(index + 1);
    materials[index] = *material;           // mirror on the CPU
    return true;
}

const std::string& MaterialPool::nameOf(uint32_t id) const {
    static const std::string none;
    return (id < names.size()) ? names[id] : none;
}
const uint32_t MaterialPool::findByName(const std::string& n) const {
    for (uint32_t i = 1; i < length && i < names.size(); i++)
        if (names[i] == n) return i;
    return 0;                                  // 0 = the empty sentinel
}
const uint32_t MaterialPool::findIdentical(const Material& m) const {
    for (uint32_t i = 1; i < length; i++)
        if (std::memcmp(&materials[i], &m, sizeof(Material)) == 0) return i;
    return 0;
}

const std::string MaterialPool::uniqueName(const std::string& base) const {
    if (!findByName(base)) return base;
    for (int n = 2;; n++) {
        const std::string cand = base + " (" + std::to_string(n) + ")";
        if (!findByName(cand)) return cand;
    }
}


uint32_t MaterialPool::collapse(uint32_t target, std::vector<uint32_t>& remap,
                                std::vector<std::string>* lost)
{
    remap.resize(length);
    for (uint32_t i = 0; i < length; i++) remap[i] = i;
    if (target < 2) target = 2;
    if (length <= target) return 0;

    std::vector<uint8_t> alive(length, 1);
    std::vector<double>  weight(length, 1.0);  // how many originals a slot now stands for
    alive[0] = 0;                              // slot 0 is the empty sentinel
    uint32_t live = length - 1, merges = 0;

    while (live > target - 1) {
        float best = FLT_MAX;
        uint32_t bi = 0, bj = 0;
        for (uint32_t i = 1; i < length; i++) {
            if (!alive[i]) continue;
            for (uint32_t j = i + 1; j < length; j++) {
                if (!alive[j]) continue;
                const float d = distance(materials[i], materials[j]);
                if (d < best) { best = d; bi = i; bj = j; }
            }
        }
        if (!bi || best == FLT_MAX) break;     // only incomparable pairs left

        const double wi = weight[bi], wj = weight[bj], w = wi + wj;
        Material&       a = materials[bi];
        const Material& b = materials[bj];
        a.color             = (a.color * (float)wi + b.color * (float)wj) / (float)w;
        a.specularColor     = (a.specularColor * (float)wi + b.specularColor * (float)wj) / (float)w;
        a.roughness         = (float)((a.roughness         * wi + b.roughness         * wj) / w);
        a.specular          = (float)((a.specular          * wi + b.specular          * wj) / w);
        a.metallic          = (float)((a.metallic          * wi + b.metallic          * wj) / w);
        a.emissiveIntensity = (float)((a.emissiveIntensity * wi + b.emissiveIntensity * wj) / w);
        weight[bi] = w;
        alive[bj]  = 0;
        if (lost && bj < names.size()) lost->push_back(names[bj]);
        for (uint32_t k = 0; k < length; k++)  // redirect everyone aimed at bj
            if (remap[k] == bj) remap[k] = bi;
        live--; merges++;
    }

    // Compact survivors to the front and rewrite remap into final ids.
    std::vector<uint32_t> newId(length, 0);
    std::vector<Material>    nm(1, materials[0]);
    std::vector<std::string> nn(1, names.empty() ? std::string() : names[0]);
    for (uint32_t i = 1; i < length; i++) {
        if (!alive[i]) continue;
        newId[i] = (uint32_t)nm.size();
        nm.push_back(materials[i]);
        nn.push_back(i < names.size() ? names[i] : std::string());
    }
    for (uint32_t k = 0; k < length; k++) remap[k] = newId[remap[k]];

    materials = nm;
    names     = nn;
    length    = (uint32_t)nm.size();
    return merges;
}
