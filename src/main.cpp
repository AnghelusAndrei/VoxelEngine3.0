#include "voxelengine.hpp"

int main()
{
    const Config config = { 
        .window_width=800,
        .window_height=600,
        .aspect_ratio=4.0f/3.0f,
        .window_title="VoxelEngine"
    };
    VoxelEngine engine(&config);

    engine.run();
} 