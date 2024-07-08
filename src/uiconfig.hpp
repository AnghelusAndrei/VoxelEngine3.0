#pragma once

#include <cstdint>
#include <string>

struct UIConfig {
    ImGuiIO& io;
    glm::vec3 direction;
    glm::vec3 position;
    glm::dvec2 cursor;
    float *speed;
    float *sensitivity;
};