#include "voxelengine.hpp"

int main()
{
    const Config config = { 
        .window_width=1200,
        .window_height=900,
        .aspect_ratio=4.0f/3.0f,
        .window_title="VoxelEngine"
    };
    VoxelEngine engine(&config);

    engine.run();
} 