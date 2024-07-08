#pragma once

#include <glm/vec4.hpp>

struct Material{
    glm::vec4 color;            //16 16
    float ambient;              //4 20
    float diffuse;              //4 24
    float specular;             //4 28
    float roughness;            //4 32
    float reflection;           //16 48
};
