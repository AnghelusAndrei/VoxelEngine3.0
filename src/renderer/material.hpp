#pragma once

#include <stack>
#include <vector>
#include <functional>
#include <cstdlib>

#include <glad/glad.h>

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include <glm/vec4.hpp>

struct Material{
    glm::vec4 color;            //16 (Albedo/Diffuse color)
    glm::vec4 specularColor;    //16 (Reserved for future use; set to color for metallic)
    float roughness;            //4  (0=smooth mirror, 1=fully rough)
    float specular;             //4  (Specular intensity, typically 0.04 for dielectrics)
    float metallic;             //4  (0=dielectric, 1=metal)
    GLint emissive = 0;         //4  (GLint = 4 bytes, matches std140 bool layout)
    float emissiveIntensity;    //4
    float _pad[3] = {0,0,0};   //12 (explicit std140 tail padding → sizeof == 64)
};

class MaterialPool{
    public:
        MaterialPool();
        ~MaterialPool();
        uint32_t addMaterial(Material *material);
        bool setMaterial(Material *material, uint32_t index);

        // CPU mirror of the uploaded UBO, so subsystems (e.g. LightTree) can read a
        // material's emissive/color/intensity by id without a GPU readback. Index 0 is
        // the empty sentinel; valid ids are 1..length-1.
        const Material& get(uint32_t id) const { return materials[id]; }
        bool isEmissive(uint32_t id) const { return id < materials.size() && materials[id].emissive != 0; }

        uint32_t length;
        uint32_t capacity;
        std::vector<Material> materials;   // CPU mirror, parallel to the GPU UBO

        friend class Renderer;
    private:
        void setProgram(GLuint program_);
        void GenUBO();
        void freeVRAM();

        GLuint gl_ID;
        GLuint program;
};