#include "voxelengine.hpp"
#include "Noise/FractalNoise.h"

VoxelEngine::VoxelEngine(const Config *config) : running(true){
    renderer = new Renderer(config);

    renderer->setUniformi(6, "lightSamples");
    renderer->setUniformi(6, "reflectionSamples");

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
        .depth = 6,
        .renderer = renderer
    };

    camera = new FPCamera(cameraConfig, controllerConfig);
    octree = new Octree(octreeConfig);
    materialPool = new MaterialPool(renderer->programID);
    lightPool = new LightPool(renderer);

    Light l1 = {
        .position = glm::vec4(-10, 570, 480, 0),
        .color = glm::vec4(1, 1, 1, 1),
        .radius = 30.0f,
        .intensity = 0.7f,
        .area = 130
    };

    Light l2 = {
        .position = glm::vec4(300, -30, 50, 0),
        .color = glm::vec4(1, 1, 0.4f, 1),
        .radius = 50.0f,
        .intensity = 0.7f,
        .area = 200
    };

    Light l3 = {
        .position = glm::vec4(-10, 300, -100, 0),
        .color = glm::vec4(1, 0.6f, 1, 1),
        .radius = 40.0f,
        .intensity = 0.7f,
        .area = 200
    };

    Light l4 = {
        .position = glm::vec4(570, 400, -100, 0),
        .color = glm::vec4(1, 0.7f, 0.9f, 1),
        .radius = 16.0f,
        .intensity = 0.7f,
        .area = 200
    };

    //lightPool->addLight(&l1);
    lightPool->addLight(&l2);
    lightPool->addLight(&l3);
    //lightPool->addLight(&l4);

    uint32_t mats[11][11] = {0};

    for(int i = 0; i < 11; i++){
        for(int j = 0; j < 11; j++){
            Material m = {
                .color = glm::vec4((float)i/10, (float)j/10, 1.0f, 0.0f),
                .ambient = 0.1f,
                .diffuse = 0.8f,
                .specular = 50.0f,
                .roughness = 0.4f,
                .reflection = 0.3f,
            };
            mats[i][j] = materialPool->addMaterial(&m);
        }
    }


	Perlin *noiseMaker = new Perlin();

    uint32_t octree_length = 1 << octree->depth;
    uint32_t normal_samples = 3;
    
    for(int i = 0; i < octree_length && !glfwWindowShouldClose(renderer->window); i++){
        for(int j = 0; j < octree_length; j++){
            for(int k = 0; k < octree_length; k++){

                float criteria = noiseMaker->noise((float)i * 0.03f,(float)j * 0.03f,(float)k * 0.03f);
                if(criteria < 0.15)continue;

                int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                glm::vec3 normal = glm::vec3(0,0,0);

                for(int a = ((i-normal_samples) > 0 ? (i-normal_samples) : 0); a <= i+normal_samples && a < octree_length; a++) 
                for(int b = ((j-normal_samples) > 0 ? (j-normal_samples) : 0); b <= j+normal_samples && b < octree_length; b++) 
                for(int c = ((k-normal_samples) > 0 ? (k-normal_samples) : 0); c <= k+normal_samples && c < octree_length; c++){
                            float local_criteria = noiseMaker->noise((float)a * 0.03f,(float)b * 0.03f,(float)c * 0.03f);
                            if(local_criteria < 0.15 && a > 0 && a < octree_length && b > 0 && b < octree_length && c > 0 && c < octree_length)
                                normal += glm::vec3(a-i,b-j,c-k);     
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

    while(!glfwWindowShouldClose(renderer->window)){
        run();
    }
}

void VoxelEngine::run(){
    glfwPollEvents();

    bool stationary = true;
    if(!renderer->ui->io.WantCaptureMouse && !renderer->ui->io.WantCaptureKeyboard)
        stationary = !camera->GLFWInput(renderer->window);
    
    if(stationary) staticFrames++;
    else staticFrames = 0;

    glfwGetCursorPos(renderer->window, &renderer->ui->cursor.x, &renderer->ui->cursor.y);
    renderer->ui->position = camera->position;
    renderer->ui->direction = camera->direction;
    renderer->ui->speed = &camera->config->speed;
    renderer->ui->sensitivity = &camera->config->sensitivity;

    renderer->run();
}

VoxelEngine::~VoxelEngine(){
    delete camera;
    delete octree;
    delete lightPool;
    delete materialPool;
    delete renderer;
}