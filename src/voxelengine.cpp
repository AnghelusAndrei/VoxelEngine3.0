#include "voxelengine.hpp"
#include "Noise/FractalNoise.h"

VoxelEngine::VoxelEngine(const Config *config) : running(true){
    renderer = new Renderer(config);

    renderer->setUniformi(1, "diffuseSamples");
    renderer->setUniformi(1, "reflectionSamples");

    renderer->setUniformf(0.4, "skyboxDiffuseIntensity");
    renderer->setUniformf(0.2, "skyboxSpecularIntensity");

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
    materialPool = new MaterialPool(renderer->programID);

    uint32_t mats[11][11] = {0};

    for(int i = 0; i < 11; i++){
        for(int j = 0; j < 11; j++){
            if(i == 5 && j == 6){
                Material emissive_m = {
                    .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
                    .ambient = 0.1f,
                    .diffuse = 0.8f,
                    .specular = 50.0f,
                    .roughness = 0.4f,
                    .reflection = 0.3f,
                    .shininess = 50.0f,
                    .emissive = true,
                    .intensity = 40.0f
                };
                mats[5][6] = materialPool->addMaterial(&emissive_m);
            }else{
                Material m = {
                    .color = glm::vec4((float)i/10, (float)j/10, 1.0f, 0.0f),
                    .ambient = 0.01f,
                    .diffuse = 0.8f,
                    .specular = 0.6f,
                    .roughness = 0.4f,
                    .reflection = 0.3f,
                    .shininess = 50.0f,
                    .emissive = false
                };
                mats[i][j] = materialPool->addMaterial(&m);
            }
        }
    }

	Perlin *noiseMaker = new Perlin();

    uint32_t octree_length = 1 << octree->depth;
    uint32_t normal_samples = 2;
    
    for(int i = 0; i < octree_length && !glfwWindowShouldClose(renderer->window); i++){
        for(int j = 0; j < octree_length; j++){
            for(int k = 0; k < octree_length; k++){

                float criteria = noiseMaker->noise((float)i * 0.03f,(float)j * 0.03f,(float)k * 0.03f);
                if(criteria < 0.15)continue;

                int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                glm::vec3 normal = glm::vec3(0,0,0);

                if(noiseMaker->noise((float)(i + 1) * 0.03f,(float)j * 0.03f,(float)k * 0.03f) < 0.15 || 
                    noiseMaker->noise((float)(i - 1) * 0.03f,(float)j * 0.03f,(float)k * 0.03f) < 0.15 || 
                    noiseMaker->noise((float)(i) * 0.03f,(float)(j + 1) * 0.03f,(float)k * 0.03f) < 0.15 || 
                    noiseMaker->noise((float)(i) * 0.03f,(float)(j - 1) * 0.03f,(float)k * 0.03f) < 0.15 || 
                    noiseMaker->noise((float)(i) * 0.03f,(float)j * 0.03f,(float)(k + 1) * 0.03f) < 0.15 || 
                    noiseMaker->noise((float)(i) * 0.03f,(float)j * 0.03f,(float)(k - 1) * 0.03f) < 0.15){
                for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                for(int c = k-normal_samples; c <= k+normal_samples; c++){
                            float local_criteria = noiseMaker->noise((float)a * 0.03f,(float)b * 0.03f,(float)c * 0.03f);
                            if(local_criteria < 0.15 && a > 0 && a < octree_length && b > 0 && b < octree_length && c > 0 && c < octree_length)
                                normal += glm::vec3(a-i,b-j,c-k);     
                }  
                normal = glm::normalize(normal);
                }
                else 
                    normal = glm::vec3(1,0,0); 
                    

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
    delete materialPool;
    delete renderer;
}