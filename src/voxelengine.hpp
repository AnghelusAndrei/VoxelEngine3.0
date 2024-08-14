#pragma once

#include "./renderer/renderer.hpp"
#include "./UI/interface.hpp"
#include "./UI/logger.hpp"
#include "./UI/info.hpp"
#include "./UI/control.hpp"
#include "./UI/viewportWidget.hpp"
#include "fpcamera.hpp"

class VoxelEngine{
    public:
        struct Config{
            glm::ivec2 windowSize;
            float viewportAspectRatio;
            const char* windowName;
        };
    public:
        VoxelEngine(const Config *windowConfig);
        ~VoxelEngine();

        void setupContext(const Config *windowConfig);
        void static glfw_error_callback(int error, const char* description);
    private:    
        GLFWwindow *window;
        Renderer *renderer;

        FPCamera *camera;
        Octree *octree;
        MaterialPool *materialPool;
        Interface *interface;

        bool ui_active = true;
};