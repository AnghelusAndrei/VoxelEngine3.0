#pragma once

#include <stack>
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
};

class MaterialPool{
    public:
        MaterialPool();
        ~MaterialPool();
        uint32_t addMaterial(Material *material);
        bool setMaterial(Material *material, uint32_t index);

        uint32_t length;
        uint32_t capacity;

        friend class Renderer;
    private:
        void setProgram(GLuint program_);
        void GenUBO(GLuint program_);
        void freeVRAM();

        GLuint gl_ID;
        GLuint program;
};