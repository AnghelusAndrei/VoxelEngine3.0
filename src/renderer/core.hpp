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
// SSBO binding points — shared between the C++ glBindBufferBase calls and the
// GLSL `layout(std430, binding = N)` qualifiers. Keep these in sync with the
// numeric literals in the shaders (GLSL can't include a C++ header).
//   0  octree node buffer   (uvec2[] : lo/hi per node)
//   1  octree claim bitfield (uint[]  : 1 bit per slot, atomicOr dedup — next stage)
//   2  lBuffer radiance cache (uint[] : see architecture/LBUFFER.md)
// -----------------------------------------------------------------------------
constexpr GLuint SSBO_OCTREE_BINDING       = 0;
constexpr GLuint SSBO_CLAIM_BINDING        = 1;
constexpr GLuint SSBO_LBUFFER_BINDING      = 2;   // flat open-addressed voxel hash (see CLAIM.md)
constexpr GLuint SSBO_UNIQUE_LIST_BINDING  = 3;   // uvec4[] : {key.xy, claimedSlot, spare}
constexpr GLuint SSBO_UNIQUE_COUNT_BINDING = 4;   // uint    : atomic append counter
constexpr GLuint SSBO_INDIRECT_ARGS_BINDING = 8;  // uvec4[] : dispatch-indirect args
// bindings 5/6 are free (were the emissive light SVO + ReSTIR reservoir — removed with the NEE path).
// bindings 6/7 (ping-pong retry) and 9 (per-bucket lock) retired with the lock-free
// open-addressing claim — slots are claimed by CAS on the timestamp word, single dispatch.

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
// GPU pass descriptors — lightwei// Shader handles are not retained after link; only the program handle is kept.
// -----------------------------------------------------------------------------
struct ComputePass {
    GLuint     program   = 0;
    glm::ivec2 groupSize;
    glm::ivec2 globalSize;
    GLuint     texture   = 0;
};

struct RasterPass {
    GLuint program     = 0;
    GLuint VBO         = 0;
    GLuint VAO         = 0;
    GLuint framebuffer = 0;
    GLuint rbo         = 0;
    GLuint texture     = 0;
};

// -----------------------------------------------------------------------------
// DispatchArgs — covers both glDispatchCompute and glDispatchComputeIndirect.
// Use shader::dispatch(args) to issue the call.
// -----------------------------------------------------------------------------
enum class DispatchMode : uint8_t { Direct, Indirect };

struct DispatchArgs {
    DispatchMode mode = DispatchMode::Direct;
    union {
        struct { glm::ivec2 globalSize; glm::ivec2 groupSize; } direct;
        struct { GLuint buffer; GLintptr offset;               } indirect;
    };

    static DispatchArgs makeDirect(glm::ivec2 global, glm::ivec2 group) noexcept {
        DispatchArgs a{};
        a.mode              = DispatchMode::Direct;
        a.direct.globalSize = global;
        a.direct.groupSize  = group;
        return a;
    }
    static DispatchArgs makeIndirect(GLuint buf, GLintptr off = 0) noexcept {
        DispatchArgs a{};
        a.mode             = DispatchMode::Indirect;
        a.indirect.buffer  = buf;
        a.indirect.offset  = off;
        return a;
    }
};

// -----------------------------------------------------------------------------
// RenderType — selects the resolve visualisation mode. Values are passed to
// resolve.comp as the `renderMode` uniform; keep in sync with the constants there.
// -----------------------------------------------------------------------------
enum RenderType {
    OCTREE        = 0,   // depth/size-shaded
    MATERIAL      = 1,   // material albedo
    NORMAL        = 2,   // decoded lBuffer normal (Increment 3)
    VERSION       = 3,   // per-voxel version (debug)
    CLAIM_AGE     = 4,   // frame - lBuffer timestamp (debug, Increment 2)
    LRU_OCCUPANCY = 5,   // lBuffer hit/bucket fill (debug, Increment 2)
    VIRTUAL       = 6,   // downscaled virtual-gbuffer material (debug, B1.1)
    SHADE         = 7,   // raw 1-spp path-traced virtual buffer (debug, B1.2)
    SHADING       = 8,   // final lit composite from the denoised cache (B1.3/B1.4)
    SAMPLES       = 9,   // per-voxel diffuse EMA sample count heatmap (debug)
    STEPS         = 10   // per-ray DDA step count heatmap (primary.comp cost, debug)
};

// -----------------------------------------------------------------------------
// FrameConfig — per-frame parameters set by the application.
// -----------------------------------------------------------------------------
struct FrameConfig {
    RenderType renderType          = SHADING;
    bool       shaderRecompilation = false;
    bool       renderToTexture     = false;
    int        primary_raystop     = 100;
    int        normalPrecision     = 6;     // normal kernel radius (voxel-size units)
    int        lbuffer_retries     = 6;     // lbuffer_claim ping-pong iterations (tunable)
    int        virtualScale        = 5;     // shading downscale divisor (virtual framebuffer)
    int        emaDiffuse          = 28;    // diffuse temporal window (sample cap, long)
    int        emaSpecular         = 1;     // specular temporal window (sample cap, short)
    int        atrousIters         = 4;     // à-trous MAX iters on virtual SPECULAR (adaptive by roughness: mirrors→1, glossy→this); 0 = off
    float      atrousSigmaN        = 80.0f; // à-trous normal edge-stop exponent (higher = sharper)
    float      atrousSigmaP        = 2.0f;  // à-trous position edge-stop (center-voxel-size units)
    int        staleFrames         = 64;    // re-visit gap (frames) past which a cached channel's EMA count is reset (diffuse + specular; 0 = off)
};

// -----------------------------------------------------------------------------
// FrameStats — renderer output exposed to the UI each frame.
// Written by Renderer::run(), read by the Info widget.
// -----------------------------------------------------------------------------
struct FrameStats {
    uint32_t  voxels_num      = 0;
    uint32_t  scene_capacity  = 0;
    uint32_t  scene_mem       = 0;
    uint32_t  lBuffer_mem     = 0;

    // Camera snapshot (for display)
    glm::vec3 cam_position;
    glm::vec3 cam_direction;
};

} // namespace core
