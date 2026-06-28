#pragma once

#include "./renderer/renderer.hpp"
#include "./UI/interface.hpp"
#include "./UI/logger.hpp"
#include "./UI/info.hpp"
#include "./UI/control.hpp"
#include "./UI/viewportWidget.hpp"
#include "./renderer/skybox.hpp"
#include "./renderer/lighttree.hpp"
#include "fpcamera.hpp"

class VoxelEngine{
    public:
        struct Config{
            glm::ivec2  windowSize;
            float       viewportAspectRatio;
            const char* windowName;
        };

        VoxelEngine(const Config *windowConfig);
        ~VoxelEngine();

        // Runs the main loop. Call once after construction; returns when the
        // window is closed.
        void run();

        void setupContext(const Config *windowConfig);
        static void glfw_error_callback(int error, const char* description);

    private:
        GLFWwindow   *window       = nullptr;
        Renderer     *renderer     = nullptr;
        FPCamera     *camera       = nullptr;
        Octree       *octree       = nullptr;
        MaterialPool *materialPool = nullptr;
        LightTree    *lightTree    = nullptr;
        Skybox       *skybox       = nullptr;
        Interface    *interface_   = nullptr;

        Info    *infoWidget    = nullptr;
        Control *controlWidget = nullptr;

        core::RendererConfig       rendererConfig;
        core::FrameConfig          frameConfig;
        FPCamera::ControllerConfig controllerConfig;
        Camera::Config             cameraConfig;

        bool     ui_active     = true;
        uint32_t insertionMat  = 0;
        uint32_t insertionSize = 0;
        int insertionRadius = 0;

        // Material IDs set during construction, referenced in run().
        uint32_t matEmissive  = 0;
        uint32_t matRed       = 0;
        uint32_t matGreen     = 0;
        uint32_t matWhite     = 0;
        uint32_t matMetallic  = 0;
        uint32_t matBlue      = 0;
        uint32_t emissive_mat4 = 0;
};
