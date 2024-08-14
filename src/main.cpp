#include "voxelengine.hpp"

int main()
{
    const VoxelEngine::Config config = { 
        .windowSize=glm::ivec2(1200, 900),
        .viewportAspectRatio=4.0f/3.0f,
        .windowName="VoxelEngine"
    };
    VoxelEngine engine(&config);
} 