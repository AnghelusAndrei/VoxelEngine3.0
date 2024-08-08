#pragma once

#include "octree.hpp"

class VoxelEngine{
    public:
    VoxelEngine(const Config *config);
    void run();
    ~VoxelEngine();

    private:    
    FPCamera *camera;
    Octree *octree;
    MaterialPool *materialPool;

    Renderer *renderer;
    bool running;

    bool ui = false;
};