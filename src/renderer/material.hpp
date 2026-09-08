#pragma once

#include <stack>
#include <vector>
#include <string>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <functional>
#include <cstdlib>

#include <glad/glad.h>

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include <glm/vec4.hpp>

// Capacity is fixed by the octree node's 10-bit material field (MATERIAL_MAX in
// shd/constants.glsl). Index 0 is the empty sentinel, so 1023 usable materials.
inline constexpr uint32_t MATERIAL_MAX = 1024;

struct Material{
    glm::vec4 color;            //16 (Albedo/Diffuse color)
    glm::vec4 specularColor;    //16 (Reserved for future use; set to color for metallic)
    float roughness;            //4  (0=smooth mirror, 1=fully rough)
    float specular;             //4  (Specular intensity, typically 0.04 for dielectrics)
    float metallic;             //4  (0=dielectric, 1=metal)
    GLint emissive = 0;         //4  (GLint = 4 bytes, matches std140 bool layout)
    float emissiveIntensity;    //4
    float _pad[3] = {0,0,0};    //12 (explicit std140 tail padding → sizeof == 64)

    inline Material() = default;
    inline Material(glm::vec4 color_, glm::vec4 specularColor_, float roughness_, float specular_, float metallic_, bool emissive_, float emissiveIntensity_)
        : color(color_), specularColor(specularColor_), roughness(roughness_), specular(specular_),
          metallic(metallic_), emissive(emissive_ ? 1 : 0), emissiveIntensity(emissiveIntensity_) {}
};

class MaterialPool{
    public:
        MaterialPool();
        ~MaterialPool();
        void sync();
        uint32_t addMaterial(Material *material, const std::string& name);
        uint32_t addMaterial(Material *material) { return addMaterial(material, std::string()); }
        inline uint32_t addMaterial(glm::vec4 color, glm::vec4 specularColor, float roughness, float specular, float metallic, bool emissive, float emissiveIntensity, const std::string& name = std::string()) {
            Material m(color, specularColor, roughness, specular, metallic, emissive, emissiveIntensity);
            return addMaterial(&m, name);
        }
        bool setMaterial(Material *material, uint32_t index);
        const Material& get(uint32_t id) const { return materials[id]; }
        bool isEmissive(uint32_t id) const { return id < materials.size() && materials[id].emissive != 0; }


        const std::string& nameOf(uint32_t id) const;
        const uint32_t findByName(const std::string& n) const;
        const uint32_t findIdentical(const Material& m) const;
        // "base", then "base (2)", "base (3)", … until free.
        const std::string uniqueName(const std::string& base) const;

        static float distance(const Material& a, const Material& b) {
            if ((a.emissive != 0) != (b.emissive != 0)) return FLT_MAX;
            const glm::vec4 dc = a.color - b.color;
            float d = dc.x*dc.x + dc.y*dc.y + dc.z*dc.z;
            const float dr = a.roughness - b.roughness;
            const float dm = a.metallic  - b.metallic;
            const float ds = a.specular  - b.specular;
            d += dr*dr + dm*dm + ds*ds;
            if (a.emissive != 0) {                     // compare intensity in ratio
                const float hi = std::max(a.emissiveIntensity, b.emissiveIntensity);
                if (hi > 0.0f) {
                    const float di = (a.emissiveIntensity - b.emissiveIntensity) / hi;
                    d += di * di;
                }
            }
            return d;
        }
        
        // Merge the nearest material pairs until at most `target` remain. `remap` maps
        // every OLD id to its new one and MUST be applied to geometry that already
        // references the pool - see Octree::remapMaterials. O(n^3); a one-shot for a
        // pool that has actually filled, not a per-frame operation.
        uint32_t collapse(uint32_t target, std::vector<uint32_t>& remap,
                          std::vector<std::string>* lost = nullptr);

        uint32_t length;
        uint32_t capacity;
        std::vector<Material> materials;   // CPU mirror, parallel to the GPU UBO
        std::vector<std::string> names;       // parallel to materials; names[0] unused

        friend class Renderer;
    private:
        void GenUBO();
        void freeVRAM();

        GLuint gl_ID;
};