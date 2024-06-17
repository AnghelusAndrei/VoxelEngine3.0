#include "voxelengine.hpp"

VoxelEngine::VoxelEngine(const Config *config) : running(true){
    _renderer = new Renderer(config);

    std::thread main_thread(run);
    std::thread rendering_thread(&Renderer::run, _renderer);
}

void VoxelEngine::run(){ 
}

VoxelEngine::~VoxelEngine(){
    main_thread.join();
    rendering_thread.join();
    delete _renderer;
}