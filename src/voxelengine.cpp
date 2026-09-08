#include "voxelengine.hpp"
#include <algorithm>
#include <vector>
#include <glm/common.hpp>
#include "./Noise/FractalNoise.h"
#include "scene.hpp"


VoxelEngine::VoxelEngine(const Config *windowConfig){
    setupContext(windowConfig);

    infoWidget    = new Info("info");
    controlWidget = new Control("control");

    auto logMessage = [this](const char* message) {
        infoWidget->AddLog("%s", message);
        fputs(message, stdout);
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
        .depth = 10
    };

    controllerConfig = {
        .speed       = 100.0f,
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
        .normalPrecision     = 6
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    interface_ = new Interface(window, renderer->glsl_version);


    infoWidget->setData(&renderer->profiler, &renderer->stats);
    controlWidget->SetConfigs(&rendererConfig, &frameConfig, &controllerConfig, &cameraConfig);


    const uint32_t L = 1u << octreeConfig.depth;

    scene::Model sponza = scene::load("./assets/models/sponza/sponza.obj",
                                      /*resolution*/ 1000,
                                      /*solid*/      false,
                                      /*thickness*/  3);
    if (sponza.ok()) {
        sponza.finish("floor",      0.35f, 0.25f, 0.0f);
        sponza.finish("Material__57", 0.30f, 0.30f, 0.0f);
        sponza.finish("fabric_a",   0.85f, 0.02f, 0.0f);
        sponza.finish("fabric_c",   0.85f, 0.02f, 0.0f);
        sponza.finish("fabric_e",   0.85f, 0.02f, 0.0f);
        sponza.finish("leaf",       0.90f, 0.00f, 0.0f);
        sponza.finish("chain",      0.30f, 0.60f, 0.9f);

        const glm::ivec3 origin(0, 0, 0);
        scene::place(sponza, *octree, *materialPool, origin);
        sponzaCentre = origin + sponza.dim / 2;
    } else {
        printf("[scene] sponza import failed: %s\n", sponza.error.c_str());
        sponzaCentre = glm::ivec3((int)L / 2);
    }

    matRed      = materialPool->addMaterial({1.0f, 0.1f, 0.1f, 0.0f}, {0.8f, 0.1f, 0.1f, 1.0f}, 0.1f, 0.5f, 0.1f, false, 0.0f, "brush.red");
    matGreen    = materialPool->addMaterial({0.1f, 1.0f, 0.1f, 0.0f}, {0.1f, 0.8f, 0.1f, 1.0f}, 0.1f, 0.0f, 0.0f, false, 0.0f, "brush.green");
    matBlue     = materialPool->addMaterial({0.1f, 0.1f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.0f, 0.0f, false, 0.0f, "brush.blue");
    matWhite    = materialPool->addMaterial({1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.6f, 0.0f, 0.0f, false, 0.0f, "brush.white");
    matMetallic = materialPool->addMaterial({0.8f, 0.8f, 0.8f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.1f, 0.95f, 0.85f, false, 0.0f, "brush.metal");
    matEmissive = materialPool->addMaterial({1.0f, 0.95f, 0.85f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.4f, 0.0f, 0.0f, true, 40.0f, "brush.emissive");
    octree->insertSphere(glm::vec3(sponzaCentre), (float)L / 40.0f, matEmissive);
    octree->insertSphere(glm::vec3(sponzaCentre) + glm::vec3(0, sponza.dim.y / 3, 0), (float)L / 40.0f, matMetallic);

    octree->flushEdits();
    octree->editLoggingEnabled = true;

    printf("[scene] %s built in %.2fs: %lu voxels, %u/%u material slots, "
           "octree %u nodes = %.0f MB (x2, CPU + VRAM)\n",
           "sponza", glfwGetTime(), (unsigned long)octree->numVoxels,
           materialPool->length, materialPool->capacity,
           octree->capacity,
           (double)octree->capacity * sizeof(Octree::Node) / 1048576.0);

    insertionMat  = matEmissive;
    insertionSize = 0;
    insertionRadius = 14;

}

void VoxelEngine::run(){
    int esc_key_delay = 200; // frames to wait before allowing ESC to toggle UI again
    int esc_key_timer = 0;
    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();

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
            }
        }

        if     (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS){ insertionMat = matEmissive; insertionSize = 0; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS){ insertionMat = matRed; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS){ insertionMat = matGreen; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS){ insertionMat = matWhite; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_5) == GLFW_PRESS){ insertionMat = matMetallic; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_6) == GLFW_PRESS){ insertionMat = matBlue; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_7) == GLFW_PRESS){ insertionMat = matWhite; insertionSize = 8; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_8) == GLFW_PRESS){ insertionMat = matWhite; insertionSize = 7; insertionRadius = 14; }
        else if(glfwGetKey(window, GLFW_KEY_9) == GLFW_PRESS){ insertionMat = matWhite; insertionSize = 6; insertionRadius = 14; }

        if(currRight && !ui_active){
            Octree::RayHit hit = octree->raycast(camera->position, camera->direction);
            if(hit.hit){
                glm::ivec3 center = glm::ivec3(hit.position) + glm::ivec3(-camera->direction);
                octree->insertSphere(center, insertionRadius, insertionMat, insertionSize);
                octree->flushEdits();
            }
        }

        esc_key_timer = std::max(0, esc_key_timer - 1);
        if(((currLeft && !interface_->io.WantCaptureMouse && !interface_->io.WantCaptureKeyboard) || (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS && esc_key_timer <= 0)) && ui_active){
            ui_active = false;
            esc_key_timer = esc_key_delay;
            camera->firstFrame = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }else if((glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS && esc_key_timer <= 0) && !ui_active){
            ui_active = true;
            esc_key_timer = esc_key_delay;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
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