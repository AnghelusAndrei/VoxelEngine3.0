#include "voxelengine.hpp"
#include <algorithm>
#include <vector>
#include <glm/common.hpp>
#include "./Noise/FractalNoise.h"


VoxelEngine::VoxelEngine(const Config *windowConfig){
    setupContext(windowConfig);

    infoWidget    = new Info("info");
    controlWidget = new Control("control");

    auto logMessage = [this](const char* format, va_list args) {
        infoWidget->vAddLog(format, args);
        vprintf(format, args);
    };

    auto fbSize = [this](){
        glm::ivec2 size;
        glfwGetFramebufferSize(window, &size.x, &size.y);
        return size;
    };

    rendererConfig = {
        .log             = logMessage,
        .framebufferSize = fbSize,
        .aspectRatio     = windowConfig->viewportAspectRatio,
        .debuggingEnabled = false
    };

    Octree::Config octreeConfig = {
        .depth = 8
    };

    controllerConfig = {
        .speed       = 40.0f,
        .sensitivity = 0.7f,
        .rotation    = glm::vec2(90, 0)
    };

    cameraConfig = {
        .position     = glm::vec3((1 << (octreeConfig.depth-1)),
                                  (1 << (octreeConfig.depth-1)),
                                  -(1 << (octreeConfig.depth-1))),
        .direction    = glm::vec3(0, 0, 1.0),
        .aspect_ratio = windowConfig->viewportAspectRatio,
        .FOV          = 90.0f
    };

    frameConfig = {
        .renderType          = core::RenderType::SHADING,
        .shaderRecompilation = false,
        .renderToTexture     = false,
        .primary_raystop     = 100,
        .normalPrecision     = 6,
        .lbuffer_retries     = 6
    };

    octree       = new Octree(&octreeConfig);
    camera       = new FPCamera(&cameraConfig, &controllerConfig);
    materialPool = new MaterialPool();
    skybox       = new Skybox("./assets/skybox",
                              "skyrender0001.bmp",
                              "skyrender0004.bmp",
                              "skyrender0006.bmp",
                              "skyrender0003.bmp",
                              "skyrender0005.bmp",
                              "skyrender0002.bmp");
    renderer = new Renderer(&rendererConfig, octree, camera, materialPool, skybox);
    lightTree = new LightTree(octree, materialPool);
    lightTree->attach();   // mirror emissive octree edits into the light tree (O(depth)/voxel)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    interface_ = new Interface(window, renderer->glsl_version);


    infoWidget->setData(&renderer->profiler, &renderer->stats);
    controlWidget->SetConfigs(&rendererConfig, &frameConfig, &controllerConfig, &cameraConfig);


    // -------------------------------------------------------------------------
    // Material definitions
    // -------------------------------------------------------------------------
    Material emissive_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = true,  .emissiveIntensity = 2.0f
    };
    Material emissive_m2 = {
        .color = glm::vec4(0.6f + 0.4f*(rand()%100)/100.0f,
                           0.6f + 0.4f*(rand()%100)/100.0f,
                           0.6f + 0.4f*(rand()%100)/100.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = true,  .emissiveIntensity = 2.0f
    };
    Material emissive_m3 = {
        .color = glm::vec4(0.5f, 0.5f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = true,  .emissiveIntensity = 2.0f
    };
    Material emissive_m4 = {
        .color = glm::vec4(0.8f, 0.5f, 0.8f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = true,  .emissiveIntensity = 100.0f
    };
    // NOTE: restored placeholder — your mid-edit had removed light_m while matLight (the emissive
    // test plates, ~line 186) still used it, breaking the build. Tune colour/intensity to taste.
    Material light_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = true,  .emissiveIntensity = 60.0f
    };
    Material red_m = {
        .color = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(0.8f, 0.1f, 0.1f, 1.0f),
        .roughness = 0.1f, .specular = 0.5f, .metallic = 0.1f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material green_m = {
        .color = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.8f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material blue_m = {
        .color = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.8f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material white_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.6f, .specular = 0.0f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material metallic_m = {
        .color = glm::vec4(0.8f, 0.8f, 0.8f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.1f, .specular = 0.95f, .metallic = 0.85f,
        .emissive = false, .emissiveIntensity = 0.0f
    };


    uint32_t emissive_mat2 = materialPool->addMaterial(&emissive_m2);
    matEmissive            = materialPool->addMaterial(&emissive_m);
    uint32_t emissive_mat3 = materialPool->addMaterial(&emissive_m3);
    emissive_mat4 = materialPool->addMaterial(&emissive_m4);
    matRed                 = materialPool->addMaterial(&red_m);
    matGreen               = materialPool->addMaterial(&green_m);
    matWhite               = materialPool->addMaterial(&white_m);
    matMetallic            = materialPool->addMaterial(&metallic_m);
    matBlue                = materialPool->addMaterial(&blue_m);
    uint32_t matLight      = materialPool->addMaterial(&light_m);

    uint32_t L = 1u << octreeConfig.depth;


    FractalNoise noiseGen = FractalNoise();
    noiseGen.setOctaves(1);
    noiseGen.setPersistence(0.4f);
    noiseGen.setLacunarity(2.0f);
    noiseGen.setBaseFrequency(0.1f);
    noiseGen.setBaseAmplitude(1.0f);


    octree->insertSphere(glm::vec3(L/3.0f, L/3.0f - 5.0f, L/2.0f), L/4.0f, matMetallic);
    octree->insertSphere(glm::vec3(L*3.0f/4.0f - 5.0f, L*3.0f/4.0f - 15.0f, L*3.0f/4.0f - 5.0f), L/4.0f, matWhite);

    octree->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(L,   4,   L), matWhite);
    octree->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(4,   L,   L), matGreen);
    octree->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(L,   L,   4), matBlue);
    octree->insertBox(glm::uvec3(L-4, 0,   0), glm::uvec3(L,   L,   L), matRed);
    octree->insertBox(glm::uvec3(0,   0, L-4), glm::uvec3(L,   L,   L), matMetallic);
    octree->insertBox(glm::uvec3(0, L-4,   0), glm::uvec3(L,   L,   L), matWhite);

    octree->insertBox(glm::uvec3(L/4, L-8, L/4), glm::uvec3(L*3/4, L-4, L*3/4), emissive_mat2);  // original soft slab
    // B2 test: 3×3 grid of THIN 1-voxel-thick emissive plates (zero interior — every emissive
    // voxel is exposed/visible from below). This isolates whether NEE itself is sound, removing
    // the solid-cluster interior-occlusion problem. (Solid spheres were pathological: most voxels
    // interior → shadow rays die on the surface → wasted samples.) Revert: restore the slab line.
    /*(void)emissive_mat2;
    for (int gx = 0; gx < 3; gx++)
    for (int gz = 0; gz < 3; gz++) {
        uint32_t cx = (uint32_t)(L*0.2f) + gx*(uint32_t)(L*0.3f);
        uint32_t cz = (uint32_t)(L*0.2f) + gz*(uint32_t)(L*0.3f);
        octree->insertBox(glm::uvec3(cx, L-6u, cz), glm::uvec3(cx+8u, L-5u, cz+8u), matLight);
    }*/

    octree->flushEdits();
    octree->editLoggingEnabled = true;

    // The light tree was populated incrementally by onLeafChanged during the scene build
    // above; just upload it (SSBO binding 5). Edits maintain it via the same hook + flushEdits.
    lightTree->GenSSBO();

    insertionMat  = matEmissive;
    insertionSize = 0;
    insertionRadius = 14;

}

void VoxelEngine::run(){
    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();

        if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS){
            ui_active = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        bool currLeft  = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS);
        bool currRight = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if(currLeft && !ui_active){
            Octree::RayHit hit = octree->raycast(camera->position, camera->direction);
            if(hit.hit){
                const int R = 14;
                glm::ivec3 center(hit.position);
                for(int dx = -R; dx <= R; dx++)
                for(int dy = -R; dy <= R; dy++)
                for(int dz = -R; dz <= R; dz++){
                    if(dx*dx + dy*dy + dz*dz > R*R) continue;
                    glm::ivec3 vp = center + glm::ivec3(dx, dy, dz);
                    if(vp.x >= 0 && vp.y >= 0 && vp.z >= 0)
                        octree->remove(glm::uvec3(vp));
                }
                octree->flushEdits();
                lightTree->flushEdits();     // emissive removals already mirrored via onLeafChanged
            }
        }

        if     (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS){ insertionMat = matEmissive; insertionSize = 0; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS){ insertionMat = matRed; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS){ insertionMat = matGreen; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS){ insertionMat = matWhite; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_5) == GLFW_PRESS){ insertionMat = matMetallic; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_6) == GLFW_PRESS){ insertionMat = matBlue; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_7) == GLFW_PRESS){ insertionMat = emissive_mat4; insertionRadius = 2; }
        else if(glfwGetKey(window, GLFW_KEY_8) == GLFW_PRESS){ insertionMat = matWhite; insertionSize = 7; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_9) == GLFW_PRESS){ insertionMat = matWhite; insertionSize = 6; insertionRadius = 14; }

        if(currRight && !ui_active){
            Octree::RayHit hit = octree->raycast(camera->position, camera->direction);
            if(hit.hit){
                glm::ivec3 center = glm::ivec3(hit.position) + glm::ivec3(-camera->direction);
                octree->insertSphere(center, insertionRadius, insertionMat, insertionSize);
                octree->flushEdits();
                lightTree->flushEdits();     // emissive inserts already mirrored via onLeafChanged
            }
        }

        if(currLeft && !interface_->io.WantCaptureMouse && !interface_->io.WantCaptureKeyboard && ui_active){
            ui_active = false;
            camera->firstFrame = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }

        if(!ui_active)
            camera->GLFWInput(window);

        if(ui_active){
            Widget *widgets[2] = {infoWidget, controlWidget};
            interface_->Draw(widgets, 2);
        }else{
            Widget *widgets[1] = {infoWidget};
            interface_->Draw(widgets, 1);
        }

        if(!renderer->run(&frameConfig))
            break;

        interface_->Render();
        glfwSwapBuffers(window);
    }
}

VoxelEngine::~VoxelEngine(){
    delete infoWidget;
    delete controlWidget;
    delete camera;
    delete lightTree;
    delete octree;
    delete materialPool;
    delete skybox;
    delete interface_;
    glfwDestroyWindow(window);
    glfwTerminate();
    delete renderer;
}

void VoxelEngine::setupContext(const Config *windowConfig){
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

    // 4.6 core: required for KHR_shader_subgroup (wave compaction in primary.comp).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(windowConfig->windowSize.x, windowConfig->windowSize.y,
                              windowConfig->windowName, nullptr, nullptr);
    if (window == nullptr)
        return;

    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(0);
}

void VoxelEngine::glfw_error_callback(int error, const char* description){
    printf("GLFW Error %d: %s\n", error, description);
}