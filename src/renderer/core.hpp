#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdint>
#include <string>
#include <stdio.h>
#include <functional>
#include <cstdarg>

#include <glad/glad.h>
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/rotate_vector.hpp>

namespace core{
    typedef std::function<void(const char*, va_list args)> logFunc;
    typedef std::function<glm::ivec2()> framebufferSizeFunc;

    struct RendererConfig {
        logFunc log;
        framebufferSizeFunc framebufferSize;
        float aspectRatio; 
        bool debuggingEnabled;

        glm::ivec2 lBufferSize = glm::ivec2(-1, -1);

        void logMessage(const char* format, ...) const {
            va_list args;
            va_start(args, format);
            log(format, args);
            va_end(args);
        }
    };

    struct ComputePass{
        GLuint shader, program;
        glm::ivec2 groupSize;
        glm::ivec2 globalSize;
        GLuint texture;
    };

    struct RasterPass{
        GLuint vertexShader, fragmentShader, program;
        GLuint VBO, VAO;
        GLuint framebuffer, rbo;
        GLuint texture;
    };

    struct lightingBuffer{
        GLuint texture;
        glm::ivec2 size;
        uint8_t stride;
        uint8_t slots;
        GLuint instruction;
        double accumulationTime;
    };

    enum RenderType{
        DEFAULT,
        STRUCTURE,
        ALBEDO,
        NORMAL,
        VOXELID,
        BUFFERSLOTS
    };

    struct FrameConfig{
        RenderType renderType = DEFAULT;

        //lighting buffer updates
        float lBufferSwapSeconds = 0.08;
        bool TAA = false;

        //raytracing
        int spp = 1;
        int bounces = 2;
        int controlchecks = 160;

        bool shaderRecompilation = false;
        bool renderToTexture = false;
        GLuint texture;
    };

    struct DebugInfo{
        //profiling
        double start_ms = 0;
        double end_ms = 0;

        //gpu
        double gpu_start_ms;
        double gpu_shaderCompilation_ms;
        double gpu_framebufferResize_ms;
        double gpu_pass1_ms;
        double gpu_pass2_ms;
        double gpu_pass3_ms;
        double gpu_end_ms;

        //cpu
        double cpu_start_ms = 0;
        double cpu_end_ms = 0;

        //mem
        uint32_t scene_capacity = 0;
        uint32_t scene_mem = 0;
        uint32_t lBuffer_mem = 0;

        //scene
        uint32_t voxels_num = 0;
        glm::vec3 cam_position;
        glm::vec3 cam_direction;
    };

    struct runtimeRendererMem{
        glm::ivec2 displaySize;
        glm::ivec2 framebufferSize;
        glm::ivec2 framebufferPos;
        float aspectRatio;
        uint8_t texturesBound;
    };
};