#include "voxelengine.hpp"
#include "Noise/FractalNoise.h"

VoxelEngine::VoxelEngine(const Config *config) : running(true){
    renderer = new Renderer(config);

    Camera::Config *cameraConfig = new Camera::Config{
        .position = glm::vec3(0.0f,0.0f,0.0f),
        .direction = glm::vec3(1.0f,0.0f,0.0f),
        .aspect_ratio = config->aspect_ratio,
        .shaderID = renderer->programID
    };

    FPCamera::ControllerConfig *controllerConfig = new FPCamera::ControllerConfig{
        .speed = 50.0f,
        .sensitivity = 0.5f
    };

    Octree::Config *octreeConfig = new Octree::Config{
        .depth = 8,
        .renderer = renderer
    };

    camera = new FPCamera(cameraConfig, controllerConfig);
    octree = new Octree(octreeConfig);
    pool = new MaterialPool(renderer->programID);

    uint32_t mats[11][11] = {0};

    for(int i = 0; i < 11; i++){
        for(int j = 0; j < 11; j++){
            Material m = {
                .color = glm::vec4((float)i/10, (float)j/10, 1.0f, 0.0f),
                .ambient = 0.3f,
                .diffuse = 0.8f,
                .specular = 60.0f,
                .roughness = 0.7f,
                .reflection = 0
            };
            mats[i][j] = pool->addMaterial(&m);
        }
    }


	Perlin *noiseMaker = new Perlin();

    uint32_t octree_length = 1 << octree->depth;
    
    for(int i = 0; i < octree_length; i++){
        for(int j = 0; j < octree_length; j++){
            for(int k = 0; k < octree_length; k++){

                float criteria = noiseMaker->noise((float)i * 0.03f,(float)j * 0.03f,(float)k * 0.03f);
                if(criteria < 0.2)continue;

                int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                glm::vec3 normal = glm::vec3(0,0,0);

                for(int a = i-2; a <= i+2; a++){
                    for(int b = j-2; b <= j+2; b++){
                        for(int c = k-2; c <= k+2; c++){
                            float local_criteria = noiseMaker->noise((float)a * 0.03f,(float)b * 0.03f,(float)c * 0.03f);
                            if(local_criteria < 0.2 || a < 0 || a > octree_length || b < 0 || b > octree_length || c < 0 || c > octree_length)
                                normal += glm::vec3(a-i,b-j,c-k);
                        }   
                    }
                }
                normal = glm::normalize(normal);

                Octree::Node leaf;
                leaf.leaf.material = mats[r][g];
                leaf.leaf.normal = Octree::packedNormal(normal);
                octree->insert(glm::uvec3(i,j,k), leaf);
            }
        }
    }

    delete noiseMaker;
}

void VoxelEngine::run(){
    while(!glfwWindowShouldClose(renderer->window)){
        glfwPollEvents();

        if(!renderer->ui->io.WantCaptureMouse && !renderer->ui->io.WantCaptureKeyboard)
            camera->GLFWInput(renderer->window);

        glfwGetCursorPos(renderer->window, &renderer->ui->cursor.x, &renderer->ui->cursor.y);
        renderer->ui->position = camera->position;
        renderer->ui->direction = camera->direction;
        renderer->ui->speed = &camera->config->speed;
        renderer->ui->sensitivity = &camera->config->sensitivity;

        renderer->run();
    }
}

VoxelEngine::~VoxelEngine(){
    delete camera;
    delete octree;
    delete pool;
    delete renderer;
}