#pragma once

#include "renderer.hpp"

class VoxelEngine{
    public:
    VoxelEngine(const Config *config);
    void run();
    ~VoxelEngine();

    private:
    Renderer *_renderer;
    bool running;
};