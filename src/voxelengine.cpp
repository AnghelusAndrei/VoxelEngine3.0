#include "voxelengine.hpp"
#include "octree_cpu.hpp"
#include <algorithm>
#include <vector>


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
        .debuggingEnabled = false
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
        .lBufferSwapSeconds = 0.14,
        .TAA = false,
        .spp = 1,
        .bounces = 2,
        .controlchecks = 300,
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

    // -----------------------------------------------------------------------
    // Scene construction via OctreeCPU
    // -----------------------------------------------------------------------
    OctreeCPU* scene = nullptr;   // kept alive for interactive editing
    
    Material emissive_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.3f,
        .specular = 0.4f,
        .metallic = 0.3f,
        .emissive = true,
        .emissiveIntensity = 4.0f
    };
    Material red_m = {
        .color = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.7f, .specular = 0.6f, .metallic = 0.3f,
        .emissive = false, .emissiveIntensity = 0.01f
    };
    Material green_m = {
        .color = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.7f, .specular = 0.6f, .metallic = 0.3f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material white_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.8f, .specular = 0.7f, .metallic = 0.3f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material metallic_m = {
        .color = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .diffuse = 0.01f, .specular = 0.9f, .metallic = 0.9f,
        .emissive = false, .emissiveIntensity = 0.0f
    };

    uint32_t emissive_mat = materialPool->addMaterial(&emissive_m);
    uint32_t red_mat      = materialPool->addMaterial(&red_m);
    uint32_t green_mat    = materialPool->addMaterial(&green_m);
    uint32_t white_mat    = materialPool->addMaterial(&white_m);
    uint32_t metallic_mat = materialPool->addMaterial(&metallic_m);

    uint32_t L = 1u << octreeConfig.depth;

    scene = new OctreeCPU(octreeConfig.depth);

    
    scene->insertSphere(glm::vec3(L/3.0f, L/3.0f - 5.0f, L/2.0f), L/3.0f - 10.0f, metallic_mat);
    scene->insertSphere(glm::vec3(L*3.0f/4.0f - 5.0f, L*3.0f/4.0f - 15.0f, L*3.0f/4.0f - 5.0f), L/4.0f, white_mat);

    scene->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(L, 4,   L), white_mat);// Floor (y = 0..3)
    scene->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(4, L,   L), green_mat);// Left wall (x = 0..3)
    scene->insertBox(glm::uvec3(L-4, 0,   0), glm::uvec3(L, L,   L), red_mat);// Right wall (x = L-4..L-1)
    scene->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(L, L,   4), metallic_mat);// mirror (z = 0..3)
    scene->insertBox(glm::uvec3(0,   L-4, 0), glm::uvec3(L, L,   L), white_mat);// Ceiling (y = L-4..L-1)
    scene->insertBox(glm::uvec3(L/4,   L-8, L/4), glm::uvec3(L*3/4, L-4, L*3/4), emissive_mat);// Emissive light panel just below the ceiling
    
    //scene->insert(glm::uvec3(0,   0,   0), red_mat);
    octree->set(scene);
    

    renderer->debug.start_ms = glfwGetTime()*1000.0;
    renderer->debug.end_ms = glfwGetTime()*1000.0;

    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();

        if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS){
            ui_active = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        bool currLeft = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        bool currRight = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if(currLeft && !ui_active && scene){
            OctreeCPU::RayHit hit = scene->raycast(camera->position, camera->direction);
            if(hit.hit){
                const int R = 14;
                glm::ivec3 center(hit.position);

                // 1. Remove voxels in the dig sphere.
                for(int dx = -R; dx <= R; dx++)
                for(int dy = -R; dy <= R; dy++)
                for(int dz = -R; dz <= R; dz++){
                    if(dx*dx + dy*dy + dz*dz > R*R) continue;
                    glm::ivec3 vp = center + glm::ivec3(dx, dy, dz);
                    if(vp.x >= 0 && vp.y >= 0 && vp.z >= 0)
                        scene->remove(glm::uvec3(vp));
                }

                octree->applyEdits(scene);
            }
        }

        if(currRight && !ui_active && scene){
            OctreeCPU::RayHit hit = scene->raycast(camera->position, camera->direction);
            if(hit.hit){
                const int R = 14;
                glm::ivec3 center = glm::ivec3(hit.position) + glm::ivec3(-camera->direction);

                // 1. Insert voxels in the dig sphere.
                for(int dx = -R; dx <= R; dx++)
                for(int dy = -R; dy <= R; dy++)
                for(int dz = -R; dz <= R; dz++){
                    if(dx*dx + dy*dy + dz*dz > R*R) continue;
                    glm::ivec3 vp = center + glm::ivec3(dx, dy, dz);
                    if(vp.x >= 0 && vp.y >= 0 && vp.z >= 0)
                        scene->insert(glm::uvec3(vp), red_mat);
                }

                octree->applyEdits(scene);
            }
        }

        if(currLeft && !interface->io.WantCaptureMouse && !interface->io.WantCaptureKeyboard && ui_active){
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
            interface->Draw(nullptr, 0);
        }

        if(!renderer->run(&frameConfig))
            break;

        interface->Render();

        glfwSwapBuffers(window);
    }

    delete scene;
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