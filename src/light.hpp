#pragma once

#include <stack>
#include <functional>
#include <cstdlib>

#include "skybox.hpp"

struct Light{
    glm::vec4 position; //16
    glm::vec4 color;    //16
    float radius;       //4
    float intensity;    //4
    float area;         //4
    //padding           //4
};

class LightPool{
    public:
        LightPool(Renderer *renderer_);
        ~LightPool();
        uint32_t addLight(Light *light);
        bool setLight(Light *light, uint32_t index);
        void removeLight();

        uint32_t length;
        uint32_t capacity;
    private:
        Renderer *renderer;
        GLuint gl_ID;
        GLuint shaderID;
};