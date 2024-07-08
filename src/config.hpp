#pragma once

#include <cstdint>
#include <string>

struct Config {
    int window_width;
    int window_height;
    float aspect_ratio;
    const char* window_title;   
    bool debugging_enabled;
};