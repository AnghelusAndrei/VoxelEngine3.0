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
        .position = glm::vec3((1 << (octreeConfig.depth-1)), (1 << (octreeConfig.depth-1)), -(1 << (octreeConfig.depth-1))),
        .direction = glm::vec3(0, 0, 1.0),
        .aspect_ratio = windowConfig->viewportAspectRatio,
        .FOV = 90.0f
    };

    core::FrameConfig frameConfig = {
        .renderType = core::RenderType::DEFAULT,
        .primary_controlchecks = 70,
        .bounce_controlchecks  = 120,
        .reconstructionRadius  = 10,
        .normalPrecision = 6,
        .shaderRecompilation = false,
        .renderToTexture = false
    };

    octree = new Octree(&octreeConfig);
    camera = new FPCamera(&cameraConfig, &controllerConfig);
    materialPool = new MaterialPool();
    skybox = new Skybox("./assets/skybox", 
                        "skyrender0001.bmp", 
                        "skyrender0004.bmp", 
                        "skyrender0006.bmp", 
                        "skyrender0003.bmp", 
                        "skyrender0005.bmp", 
                        "skyrender0002.bmp");
    renderer = new Renderer(&rendererConfig, octree, camera, materialPool, skybox);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    interface = new Interface(window, renderer->glsl_version);

    info->setData(&renderer->profiler, &renderer->stats);
    control->SetConfigs(&rendererConfig, &frameConfig, &controllerConfig, &cameraConfig);

    // -----------------------------------------------------------------------
    // Scene construction via OctreeCPU
    // -----------------------------------------------------------------------
    OctreeCPU* scene = nullptr;   // kept alive for interactive editing
    
    Material emissive_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.4f,
        .specular = 0.03f,
        .metallic = 0.2f,
        .emissive = true,
        .emissiveIntensity = 10.0f
    };
    Material red_m = {
        .color = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(0.8f, 1.0f, 0.7f, 1.0f),
        .roughness = 0.3f, .specular = 0.9f, .metallic = 0.6f,
        .emissive = false, .emissiveIntensity = 0.01f
    };
    Material green_m = {
        .color = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.8f, .specular = 0.1f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material blue_m = {
        .color = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.8f, .specular = 0.1f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material white_m = {
        .color = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.6f, .specular = 0.2f, .metallic = 0.0f,
        .emissive = false, .emissiveIntensity = 0.0f
    };
    Material metallic_m = {
        .color = glm::vec4(0.8f, 0.8f, 0.8f, 0.0f),
        .specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .roughness = 0.15f, .specular = 1.0f, .metallic = 0.7f,
        .emissive = false, .emissiveIntensity = 0.0f
    };

    uint32_t emissive_mat = materialPool->addMaterial(&emissive_m);
    uint32_t red_mat      = materialPool->addMaterial(&red_m);
    uint32_t green_mat    = materialPool->addMaterial(&green_m);
    uint32_t white_mat    = materialPool->addMaterial(&white_m);
    uint32_t metallic_mat = materialPool->addMaterial(&metallic_m);
    uint32_t blue_mat     = materialPool->addMaterial(&blue_m);

    uint32_t L = 1u << octreeConfig.depth;

    scene = new OctreeCPU(octreeConfig.depth);

    
    scene->insertSphere(glm::vec3(L/3.0f, L/3.0f - 5.0f, L/2.0f), L/4.0f, metallic_mat);
    scene->insertSphere(glm::vec3(L*3.0f/4.0f - 5.0f, L*3.0f/4.0f - 15.0f, L*3.0f/4.0f - 5.0f), L/4.0f, white_mat);

    scene->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(L, 4,   L), white_mat);// Floor (y = 0..3)
    scene->insertBox(glm::uvec3(0,   0,   0), glm::uvec3(4, L,   L), green_mat);// Left wall (x = 0..3)
    scene->insertBox(glm::uvec3(L-4, 0,   0), glm::uvec3(L, L,   L), red_mat);// Right wall (x = L-4..L-1)
    scene->insertBox(glm::uvec3(0,   0,   L-4), glm::uvec3(L, L, L), metallic_mat);// mirror (z = 0..3)
    scene->insertBox(glm::uvec3(0,   L-4, 0), glm::uvec3(L, L,   L), white_mat);// Ceiling (y = L-4..L-1)
    scene->insertBox(glm::uvec3(L/4,   L-8, L/4), glm::uvec3(L*3/4, L-4, L*3/4), emissive_mat);// Emissive light panel just below the ceiling
    
    octree->set(scene);

    // Motion detection for temporal accumulation. We compare cam pos / dir
    // each frame; any delta above epsilon means "moving" and the shade.comp
    // EMA reverts to fast-response mode. `stationaryFrames` counts consecutive
    // still frames so we only engage temporal accumulation after things have
    // truly settled (prevents locking in stale samples mid-move).
    glm::vec3 lastCamPos = glm::vec3(0.0f);
    glm::vec3 lastCamDir = glm::vec3(0.0f);
    int stationaryFrames = 0;
    bool motionStateInitialized = false;


    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();

        // ---------------- Motion detection ---------------------------------------
        // Treat the very first call as "moving" so the initial frame doesn't
        // accidentally lock in zeros. After that, compare against last frame's
        // camera state; any non-trivial delta resets stationaryFrames.
        double frameTime = 16.67; // ~60 fps fallback until profiler warms up
        if (const ProfilerResults* pr = renderer->profiler.getResults())
            frameTime = pr->CPU_ms;
        const float posEps = 1e-4f;
        const float dirEps = 1e-4f;
        bool moving = true;
        if (motionStateInitialized) {
            float posDelta = glm::length(camera->position  - lastCamPos);
            float dirDelta = glm::length(camera->direction - lastCamDir);
            moving = (posDelta > posEps) || (dirDelta > dirEps);
        }
        motionStateInitialized = true;
        lastCamPos = camera->position;
        lastCamDir = camera->direction;
        if (moving) stationaryFrames = 0;
        else        stationaryFrames++;

        // Decide the EMA mode. Motion state controls only `updateTime` (LRU
        // stale-reset window) and the legacy single-channel `sampleCap` knob.
        // The dual-EMA caps (sampleCapDirect / sampleCapIndirect) are LEFT
        // ALONE — those are user-driven from the UI, and overwriting them
        // every frame here would silently fight the sliders. If you want
        // motion-aware dual caps, gate the writes on a flag (e.g. a UI
        // toggle "auto-tune dual caps") so the user's manual settings win
        // when explicitly requested.
        //   MOVING          → short updateTime so newly visible voxels catch
        //                     up within a couple of frames.
        //   STATIONARY + TA → updateTime infinite + stale-reset off → samples
        //                     accumulate toward convergence. Engaged after
        //                     a few still frames so mid-move deposits are
        //                     flushed first.
        //   STATIONARY      → moderate updateTime, stale-reset on.
        if (moving) {
            frameConfig.sampleCapDirect  = 128u;
            frameConfig.sampleCapIndirect = 8u;
            frameConfig.updateTime = (GLuint)((1.0 * frameTime) / 10.0);   // previous default
        } else if (stationaryFrames > 10) { // TA mode
            frameConfig.sampleCapDirect  = 1024;
            frameConfig.sampleCapIndirect = 4096;
            if (frameConfig.sampleCapDirect < 64u) frameConfig.sampleCapDirect = 64u;
            if (frameConfig.sampleCapIndirect < 64u) frameConfig.sampleCapIndirect = 64u;
            frameConfig.updateTime = 0xFFFFFFFFu;                          // effectively infinite
            frameConfig.disableStaleReset = 1;
        } else {
            frameConfig.sampleCapDirect  = 128u;
            frameConfig.sampleCapIndirect = 64u;
            frameConfig.updateTime = (GLuint)((10.0 * frameTime) / 10.0);  // longer when still
        }

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
                const int R = 10;
                glm::ivec3 center = glm::ivec3(hit.position) + glm::ivec3(-camera->direction);

                scene->insertSphere(center, R, emissive_mat);

                octree->applyEdits(scene);
            }
        }

        if(currLeft && !interface->io.WantCaptureMouse && !interface->io.WantCaptureKeyboard && ui_active){
            ui_active = false;
            camera->firstFrame = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }

        if(!ui_active)
            camera->GLFWInput(window);

        if(ui_active){
            Widget *widgets[2] = {info, control};
            interface->Draw(widgets, 2);
        }else{
            Widget *widgets[1] = {info};
            interface->Draw(widgets, 1);
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
    delete skybox;

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