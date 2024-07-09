#pragma once

#include "material.hpp"

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
    glm::vec4 color;            //16 
    float ambient;              //4 
    float diffuse;              //4 
    float specular;             //4 
    float roughness;            //4 
    float reflection;           //4 
    //padding                   //12
};

class MaterialPool{
    public:
        MaterialPool(GLuint shaderID_);
        ~MaterialPool();
        uint32_t addMaterial(Material *material);
        bool setMaterial(Material *material, uint32_t index);

        uint32_t length;
        uint32_t capacity;
    private:

        GLuint gl_ID;
        GLuint shaderID;
};