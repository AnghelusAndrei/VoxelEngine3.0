#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
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

typedef std::function<void(const char*)> logFunc;
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
constexpr GLuint SSBO_INDIRECT_ARGS_BINDING = 5;  // uvec4[] : dispatch-indirect args
constexpr GLuint SSBO_ACCUM_DIFFUSE_BINDING  = 6;   // uvec4[] : diffuse sum + pixelCount, per uniqueList entry
constexpr GLuint SSBO_ACCUM_SPECULAR_BINDING = 7;   // uvec4[] : specular sum, per uniqueList entry
constexpr GLuint SSBO_MATERIAL_BINDING       = 8;   // Material[] : too big for a UBO (see material.cpp)
// Binding 9+ free (the retry/lock buffers retired with the lock-free open-addressing claim).

// -----------------------------------------------------------------------------
// RendererConfig — passed once at construction, immutable during a session.
// -----------------------------------------------------------------------------
struct RendererConfig {
    logFunc             log;
    framebufferSizeFunc framebufferSize;
    float               aspectRatio;
    bool                debuggingEnabled;

    void logMessage(const char* format, ...) const {
        char stackBuf[1024];
        va_list args;

        va_start(args, format);
        int needed = vsnprintf(stackBuf, sizeof(stackBuf), format, args);
        va_end(args);

        if (needed < (int)sizeof(stackBuf)) {
            log(stackBuf);
            return;
        }

        std::vector<char> heapBuf(needed + 1);
        va_start(args, format);
        vsnprintf(heapBuf.data(), heapBuf.size(), format, args);
        va_end(args);
        log(heapBuf.data());
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
    // These two FREEZE THE CACHE while selected: every pass that writes the lBuffer is
    // skipped, so the camera flies around a snapshot of what is genuinely resident
    // instead of re-seating each voxel the moment it is looked at (see Renderer::run).
    CLAIM_AGE     = 4,   // frozen: frame - lBuffer timestamp; bright = recently claimed
    LRU_OCCUPANCY = 5,   // frozen: green = resident, red = evicted/never seated
    VIRTUAL       = 6,   // downscaled virtual-gbuffer material (debug, B1.1)
    SHADE         = 7,   // raw 1-spp path-traced virtual buffer (debug, B1.2)
    SHADING       = 8,   // final lit composite from the denoised cache (B1.3/B1.4)
    SAMPLES       = 9,   // per-voxel diffuse EMA sample count heatmap (debug)
    HOLES         = 10,  // cache-hole fill coverage (debug, see resolve.comp)
    LUMINANCE     = 11,  // log(luminance) heatmap (debug, see resolve.comp)
    VARIANCE      = 12   // log(luminance) variance heatmap (debug, see resolve.comp)
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
    int        virtualScale        = 5;     // shading downscale divisor (virtual framebuffer)
    int        emaDiffuse          = 140;    // diffuse temporal window (sample cap, long)
    int        emaSpecular         = 4;     // specular temporal window (sample cap, short)
    int        staleViewDep        = 50;    // re-visit gap (frames) past which a cached channel's EMA count is reset (specular; 0 = off)
    int        staleViewIndep      = 1e7;    // re-visit gap (frames) past which a cached channel's EMA count is reset (diffuse; 0 = off)
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
