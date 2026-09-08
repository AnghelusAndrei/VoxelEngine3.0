#pragma once

#include "./renderer/renderer.hpp"
#include "./UI/interface.hpp"
#include "./UI/logger.hpp"
#include "./UI/info.hpp"
#include "./UI/control.hpp"
#include "./UI/viewportWidget.hpp"
#include "./renderer/skybox.hpp"
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

        void run();

        void setupContext(const Config *windowConfig);
        static void glfw_error_callback(int error, const char* description);

    private:
        GLFWwindow   *window       = nullptr;
        Renderer     *renderer     = nullptr;
        FPCamera     *camera       = nullptr;
        Octree       *octree       = nullptr;
        MaterialPool *materialPool = nullptr;
        Skybox       *skybox       = nullptr;
        Interface    *interface_   = nullptr;

        Info    *infoWidget    = nullptr;
        Control *controlWidget = nullptr;

        core::RendererConfig       rendererConfig;
        core::FrameConfig          frameConfig;
        FPCamera::ControllerConfig controllerConfig;
        Camera::Config             cameraConfig;

        
        // demo scene state
        bool     ui_active     = true;
        uint32_t insertionMat  = 0;
        uint32_t insertionSize = 0;
        int insertionRadius = 0;
        uint32_t matEmissive  = 0;
        uint32_t matRed       = 0;
        uint32_t matGreen     = 0;
        uint32_t matWhite     = 0;
        uint32_t matMetallic  = 0;
        uint32_t matBlue      = 0;
        glm::ivec3 sponzaCentre = glm::ivec3(0);   // atrium centre; the light sits here
};
