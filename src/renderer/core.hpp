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
#include <GLFW/glfw3.h>

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/rotate_vector.hpp>

namespace core {

typedef std::function<void(const char*, va_list args)> logFunc;
typedef std::function<glm::ivec2()> framebufferSizeFunc;

// -----------------------------------------------------------------------------
// RendererConfig — passed once at construction, immutable during a session.
// -----------------------------------------------------------------------------
struct RendererConfig {
    logFunc             log;
    framebufferSizeFunc framebufferSize;
    float               aspectRatio;
    bool                debuggingEnabled;

    void logMessage(const char* format, ...) const {
        va_list args;
        va_start(args, format);
        log(format, args);
        va_end(args);
    }
};

// -----------------------------------------------------------------------------
// GPU pass descriptors — lightweight handle bundles. No logic, no ownership.
// -----------------------------------------------------------------------------
struct ComputePass {
    GLuint shader, program;
    glm::ivec2 groupSize;
    glm::ivec2 globalSize;
    GLuint texture;
};

struct RasterPass {
    GLuint vertexShader, fragmentShader, program;
    GLuint VBO, VAO;
    GLuint framebuffer, rbo;
    GLuint texture;
};

struct hashBuffer {
    GLuint     texture;
    glm::ivec2 size;
    uint8_t    stride;
    uint8_t    slots;
};

// -----------------------------------------------------------------------------
// RenderType — selects the resolve.comp visualisation mode.
// -----------------------------------------------------------------------------
enum RenderType {
    DEFAULT,
    STRUCTURE,
    ALBEDO,
    NORMAL,
    VOXELID,
    VARIANCE
};

// -----------------------------------------------------------------------------
// FrameConfig — per-frame parameters set by the application.
// -----------------------------------------------------------------------------
struct FrameConfig {
    RenderType renderType           = DEFAULT;
    int        primary_controlchecks = 70;
    int        bounce_controlchecks  = 100;
    int        reconstructionRadius  = 10;
    GLint      normalPrecision       = 6;

    bool       shaderRecompilation  = false;
    bool       renderToTexture      = false;
    GLuint     texture;
    GLint      disableStaleReset    = 0;
    GLuint     updateTime;

    int        shadingBudget        = 131072;

    // Per-channel SAMPLE_CAP for the dual-EMA lBuffer split (direct = NEE,
    // indirect = bounce GI + skybox). Direct converges in ~3 frames with
    // RIS so the cap stays low for responsive lighting changes; indirect
    // is high-variance GI so the cap stays high to average over many bounces.
    int        sampleCapDirect       = 16;
    int        sampleCapIndirect     = 64;

    // Importance-aware scheduling weights (rank.comp).
    float weightPixels   = 0.1f;
    float weightVariance = 0.9f;
    float pixelsNorm     = 100.0f;
    float varianceNorm   = 10000.0f;
    float topFraction    = 0.30f;
    float midFraction    = 0.40f;
    int   minSamples     = 1;

    // Firefly clamp (shade.comp::depositSampleDual). When a depositing sample's
    // luma exceeds K × the running per-channel mean, it is rescaled to that
    // threshold before going into the EMA. Eliminates the persistent-bright-
    // voxel artifact caused by single-sample low-PDF spikes — direct sample
    // with mean ≈ 50 + firefly of 1000 used to spike the EMA to 80 and stay
    // there for tens of frames; with K=4 and floor=32, it spikes to ≈55 and
    // decays normally over the next few samples.
    //   fireflyK     : multiplier on the running mean (K=2 strict, K=8 loose)
    //   fireflyFloor : absolute lower bound on the threshold, in raw urgb units
    //                  — keeps near-black voxels from clamping too aggressively
    float fireflyK     = 2.0f;
    float fireflyFloor = 200.0f;
};

// -----------------------------------------------------------------------------
// FrameStats — non-profiler renderer output exposed to the UI each frame.
// Written by Renderer::run(), read by the Info widget.
// -----------------------------------------------------------------------------
struct FrameStats {
    // Wavefront scheduling
    uint32_t scheduledCount = 0;
    uint32_t shadingBudget  = 0;

    // Scene
    uint32_t voxels_num     = 0;

    // Memory footprints (bytes)
    uint32_t scene_capacity = 0;
    uint32_t scene_mem      = 0;
    uint32_t lBuffer_mem    = 0;
    uint32_t nBuffer_mem    = 0;
    uint32_t rayRing_mem    = 0;
    uint32_t shadeList_mem  = 0;
    uint32_t claimMap_mem   = 0;

    // Camera snapshot (for display)
    glm::vec3 cam_position;
    glm::vec3 cam_direction;
};

} // namespace core
