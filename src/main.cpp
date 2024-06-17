#include "voxelengine.hpp"

int main()
{
    const Config config = { 
        .window_width=800,
        .window_height=600,
        .window_title="VoxelEngine"
    };
    VoxelEngine engine(&config);

    engine.run();

    delete &engine;
} 