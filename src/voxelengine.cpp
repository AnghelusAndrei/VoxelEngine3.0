#include "voxelengine.hpp"
#include "Noise/FractalNoise.h"


VoxelEngine::VoxelEngine(const Config *windowConfig){
    setupContext(windowConfig);

    Info *info = new Info("info");
    Control *control = new Control("control");

    auto logMessage = [&info](const char* format, va_list args) {
        info->vAddLog(format, args);
        vprintf(format, args);
    };

    auto fbSize = [this](){
        glm::ivec2 size; 
        glfwGetFramebufferSize(window, &size.x, &size.y); 
        return size;
    };

    core::RendererConfig rendererConfig = {
        .log = logMessage,
        .framebufferSize = fbSize,
        .aspectRatio = windowConfig->viewportAspectRatio,
        .debuggingEnabled = true
    };

    Octree::Config octreeConfig = {
        .depth = 8
    };
    
    FPCamera::ControllerConfig controllerConfig = {
        .speed = 40.0f,
        .sensitivity = 0.7f,
        .rotation = glm::vec2(-90, 0)
    };

    Camera::Config cameraConfig = {
        .position = glm::vec3((1 << (octreeConfig.depth-1)), (1 << (octreeConfig.depth-1)), (1 << (octreeConfig.depth-1)) * 3), 
        .direction = glm::vec3(0, 0, 0),
        .aspect_ratio = windowConfig->viewportAspectRatio,
        .FOV = 90.0f
    };

    core::FrameConfig frameConfig = {
        .renderType = core::RenderType::DEFAULT,
        .lBufferSwapSeconds = 0.2,
        .TAA = false,
        .spp = 1,
        .bounces = 2,
        .shaderRecompilation = false,
        .renderToTexture = false
    };


    octree = new Octree(&octreeConfig);
    camera = new FPCamera(&cameraConfig, &controllerConfig);
    materialPool = new MaterialPool();
    renderer = new Renderer(&rendererConfig, octree, camera, materialPool);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    interface = new Interface(window, renderer->glsl_version);

    info->SetProfilerData(&renderer->debug);
    control->SetConfigs(&rendererConfig, &frameConfig, &controllerConfig, &cameraConfig);

    {
        Material emissive_m = {
            .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
            .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            .diffuse = 0.3f,
            .specular = 0.4f,
            .metallic = 0.3f,
            .emissive = true,
            .emissiveIntensity = 3.0f
        };

        Material red_m = {
            .color = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
            .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            .diffuse = 0.7f,
            .specular = 0.6f,
            .metallic = 0.3f,
            .emissive = false,
            .emissiveIntensity = 0.0f
        };

        Material green_m = {
            .color = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
            .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            .diffuse = 0.7f,
            .specular = 0.6f,
            .metallic = 0.3f,
            .emissive = false,
            .emissiveIntensity = 0.0f
        };

        Material white_m = {
            .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
            .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            .diffuse = 0.8f,
            .specular = 0.7f,
            .metallic = 0.3f,
            .emissive = false,
            .emissiveIntensity = 0.0f
        };

        Material metallic_m = {
            .color = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
            .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            .diffuse = 0.01f,
            .specular = 0.9f,
            .metallic = 0.9f,
            .emissive = false,
            .emissiveIntensity = 0.0f
        };

        uint32_t emissive_mat = materialPool->addMaterial(&emissive_m);
        uint32_t red_mat = materialPool->addMaterial(&red_m);
        uint32_t green_mat = materialPool->addMaterial(&green_m);
        uint32_t white_mat = materialPool->addMaterial(&white_m);
        uint32_t metallic_mat = materialPool->addMaterial(&metallic_m);

        Perlin *noiseMaker = new Perlin();

        uint32_t octree_length = 1 << (octree->depth-1);
        uint32_t normal_samples = 3;

        glm::vec3 spherePosition = glm::vec3((float)octree_length/3.0, (float)octree_length/3.0 - 5, (float)octree_length/2.0);
        float sphereSize = (float)octree_length/3.0 - 10;

        for(int i = 0; i < octree_length && !glfwWindowShouldClose(window); i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = 0; k < octree_length; k++){
                    float d = glm::distance(glm::vec3((float)i, (float)j, (float)k), spherePosition);
                    if(d > sphereSize || d < sphereSize - 10)continue;

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                float d = glm::distance(glm::vec3((float)a, (float)b, (float)c), spherePosition);
                                if(d > sphereSize || d < sphereSize - 10)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = metallic_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        spherePosition = glm::vec3((float)octree_length*3.0/4.0 - 5, (float)octree_length*3/4 - 15, (float)octree_length*3/4 - 5);
        sphereSize = octree_length/4;

        for(int i = 0; i < octree_length && !glfwWindowShouldClose(window); i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = 0; k < octree_length; k++){
                    float d = glm::distance(glm::vec3((float)i, (float)j, (float)k), spherePosition);
                    if(d > sphereSize || d < sphereSize - 10)continue;

                    int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                    int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                float d = glm::distance(glm::vec3((float)a, (float)b, (float)c), spherePosition);
                                if(d > sphereSize || d < sphereSize - 10)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    if(r == 5 && g == 6)
                        r = 3;
                    leaf.leaf.material = white_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        for(int i = 0; i < octree_length; i++){
            for(int j = 0; j < 4; j++){
                for(int k = 0; k < octree_length; k++){

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(b >= 4 || b < 0)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = white_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        for(int i = 0; i < 4; i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = 0; k < octree_length; k++){

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(a >= 4 || a < 0)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = green_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        for(int i = octree_length-4; i < octree_length; i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = 0; k < octree_length; k++){

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(a < octree_length-4 || a >= octree_length)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = red_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        for(int i = 0; i < octree_length; i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = 0; k < 4; k++){
                    int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                    int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(c >= 4 || c < 0)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = metallic_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        /*for(int i = 0; i < octree_length; i++){
            for(int j = 0; j < octree_length; j++){
                for(int k = octree_length-5; k < octree_length; k++){
                    int r = (int)((noiseMaker->noise((float)i * 0.04f,(float)j * 0.03f,(float)k * 0.03f) + 1) * 5);
                    int g = (int)((noiseMaker->noise((float)i * 0.03f,(float)j * 0.04f,(float)k * 0.03f) + 1) * 5);

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(c < octree_length-5 || c >= octree_length)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = metallic_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }*/

        for(int i = 0; i < octree_length; i++){
            for(int j = octree_length-4; j < octree_length; j++){
                for(int k = 0; k < octree_length; k++){

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(b < octree_length-4 || b >= octree_length)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = white_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        for(int i = octree_length/4; i < octree_length*3/4; i++){
            for(int j = octree_length-8; j < octree_length-4; j++){
                for(int k = octree_length/4; k < octree_length*3/4; k++){

                    glm::vec3 normal = glm::vec3(0,0,0);

                    for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                    for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                    for(int c = k-normal_samples; c <= k+normal_samples; c++){
                                if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                    normal += glm::vec3(a-i,b-j,c-k); 
                                    continue;  
                                }
                                if(b < octree_length-8 || b >= octree_length-4)
                                    normal += glm::vec3(a-i,b-j,c-k);     
                    }  
                    normal = glm::normalize(normal);
                    if(i == octree_length-1){
                        normal += glm::vec3(1.0f, 0, 0);
                    }
                    if(j == octree_length-1){
                        normal += glm::vec3(0, 1.0f, 0);
                    }
                    if(k == octree_length-1){
                        normal += glm::vec3(0, 0, 1.0f);
                    }
                    if(i == 0){
                        normal += glm::vec3(-1.0f, 0, 0);
                    }
                    if(j == 0){
                        normal += glm::vec3(0, -1.0f, 0);
                    }if(k == 0){
                        normal += glm::vec3(0, 0, -1.0f);
                    }
                    if(normal.length() < 0.1f)
                        normal = glm::vec3(1,0,0);
                    normal = glm::normalize(normal);
                        

                    Octree::Node leaf;
                    leaf.leaf.material = emissive_mat;
                    leaf.leaf.normal = Octree::packedNormal(normal);
                    octree->insert(glm::uvec3(i,j,k), leaf);
                }
            }
        }

        delete noiseMaker;
    }

    renderer->debug.start_ms = glfwGetTime()*1000.0;
    renderer->debug.end_ms = glfwGetTime()*1000.0;

    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();


        if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS){
            ui_active = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        if(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && !interface->io.WantCaptureMouse && !interface->io.WantCaptureKeyboard){
            ui_active = false;
            camera->firstFrame = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
        if(!ui_active)
            frameConfig.TAA = !camera->GLFWInput(window);

        if(ui_active){
            Widget *widgets[2] = {info, control};
            interface->Draw(widgets, 2);
        }else{
            Widget *widgets[0] = {};
            interface->Draw(widgets, 0);
        }
        

        if(!renderer->run(&frameConfig))
            break;

        interface->Render();

        glfwSwapBuffers(window);
    }

    delete info;
}

VoxelEngine::~VoxelEngine(){
    delete camera;
    delete octree;
    delete materialPool;

    delete interface;
    glfwDestroyWindow(window);
    glfwTerminate();
    delete renderer;
}

void VoxelEngine::setupContext(const Config *windowConfig){
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    
    window = glfwCreateWindow(windowConfig->windowSize.x, windowConfig->windowSize.y, windowConfig->windowName, nullptr, nullptr);
    if (window == nullptr)
        return;

    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(0);
}

void VoxelEngine::glfw_error_callback(int error, const char* description){
    printf("GLFW Error %d: %s\n", error, description);
} 