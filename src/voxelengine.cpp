#include "voxelengine.hpp"
#include "Noise/FractalNoise.h"

VoxelEngine::VoxelEngine(const Config *config) : running(true){
    renderer = new Renderer(config);

    //renderer->setUniformi(1, "spp");
    //renderer->setUniformi(2, "lightBounces");

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

    /*
    uint32_t mats[11][11] = {0};

    for(int i = 0; i < 11; i++){
        for(int j = 0; j < 11; j++){
            if(i == 5 && j == 6){
                Material emissive_m = {
                    .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
                    .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                    .diffuse = 0.3f,
                    .specular = 0.4f,
                    .metallic = 0.3f,
                    .emissive = true,
                    .emissiveIntensity = 0.8f
                };
                mats[5][6] = materialPool->addMaterial(&emissive_m);
            }else{
                Material m = {
                    .color = glm::vec4((float)i/10, (float)j/10, 1.0f, 0.0f),
                    .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                    .diffuse = 0.8f,
                    .specular = 0.7f,
                    .metallic = 0.3f,
                    .emissive = false
                };
                mats[i][j] = materialPool->addMaterial(&m);
            }
        }
    }*/

    Material emissive_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.3f,
        .specular = 0.4f,
        .metallic = 0.3f,
        .emissive = true,
        .emissiveIntensity = 1.2f
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
    uint32_t normal_samples = 4;
    
    /*for(int i = 0; i < octree_length && !glfwWindowShouldClose(renderer->window); i++){
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
                            if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c > 0){
                                continue;  
                            }
                            float local_criteria = noiseMaker->noise((float)a * 0.03f,(float)b * 0.03f,(float)c * 0.03f);
                            if(local_criteria < 0.15)
                                normal += glm::vec3(a-i,b-j,c-k);     
                }  
                normal = glm::normalize(normal);
                }
                else {
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
                }
                    

                Octree::Node leaf;
                leaf.leaf.material = mats[r][g];
                leaf.leaf.normal = Octree::packedNormal(normal);
                octree->insert(glm::uvec3(i,j,k), leaf);
            }
        }
    }*/

    glm::vec3 spherePosition = glm::vec3((float)octree_length/3.0, (float)octree_length/3.0 - 5, (float)octree_length/2.0);
    float sphereSize = (float)octree_length/3.0 - 10;

    for(int i = 0; i < octree_length && !glfwWindowShouldClose(renderer->window); i++){
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

    for(int i = 0; i < octree_length && !glfwWindowShouldClose(renderer->window); i++){
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
        for(int j = 0; j < 5; j++){
            for(int k = 0; k < octree_length; k++){

                glm::vec3 normal = glm::vec3(0,0,0);

                for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                for(int c = k-normal_samples; c <= k+normal_samples; c++){
                            if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                normal += glm::vec3(a-i,b-j,c-k); 
                                continue;  
                            }
                            if(b >= 5 || b < 0)
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

    for(int i = 0; i < 5; i++){
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
                            if(a >= 5 || a < 0)
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

    for(int i = octree_length-5; i < octree_length; i++){
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
                            if(a < octree_length-5 || a >= octree_length)
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
            for(int k = 0; k < 5; k++){
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
                            if(c >= 5 || c < 0)
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
        for(int j = octree_length-5; j < octree_length; j++){
            for(int k = 0; k < octree_length; k++){

                glm::vec3 normal = glm::vec3(0,0,0);

                for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                for(int c = k-normal_samples; c <= k+normal_samples; c++){
                            if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                normal += glm::vec3(a-i,b-j,c-k); 
                                continue;  
                            }
                            if(b < octree_length-5 || b >= octree_length)
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
        for(int j = octree_length-7; j < octree_length-5; j++){
            for(int k = octree_length/4; k < octree_length*3/4; k++){

                glm::vec3 normal = glm::vec3(0,0,0);

                for(int a = i-normal_samples; a <= i+normal_samples; a++) 
                for(int b = j-normal_samples; b <= j+normal_samples; b++) 
                for(int c = k-normal_samples; c <= k+normal_samples; c++){
                            if(a >= octree_length || b >= octree_length || c >= octree_length || a < 0 || b < 0 || c < 0){
                                normal += glm::vec3(a-i,b-j,c-k); 
                                continue;  
                            }
                            if(b < octree_length-7 || b >= octree_length-5)
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

    while(!glfwWindowShouldClose(renderer->window)){
        run();
    }
}

void VoxelEngine::run(){
    glfwPollEvents();

    bool stationary = true;
    if(glfwGetKey(renderer->window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        ui = true;
    if(glfwGetMouseButton(renderer->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && !renderer->ui->io.WantCaptureMouse && !renderer->ui->io.WantCaptureKeyboard)
        ui = false;
    if(/*!renderer->ui->io.WantCaptureMouse && !renderer->ui->io.WantCaptureKeyboard && */!ui)
        stationary = !camera->GLFWInput(renderer->window);
    
    renderer->stationary = stationary;

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