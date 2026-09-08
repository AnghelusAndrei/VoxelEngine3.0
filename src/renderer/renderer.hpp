#pragma once
#include <array>
#include "core.hpp"
#include "shader.hpp"
#include "octree.hpp"
#include "camera.hpp"
#include "material.hpp"
#include "skybox.hpp"
#include "profiler.hpp"

class Renderer {
public:
    Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_,
             MaterialPool *materialPool_, Skybox *skybox_);
    bool run(core::FrameConfig *frameConfig);
    ~Renderer();

    core::RendererConfig *config;

    Profiler         profiler;
    core::FrameStats stats;

    const char* glsl_version = "#version 430";

private:
    // ------------------ external resources --------------------
    Octree       *volume;
    Camera       *camera;
    MaterialPool *materialPool;
    Skybox       *skybox;

    bool     success  = true;
    uint32_t frameIdx = 0;

    // CLAIM_AGE / LRU_OCCUPANCY freeze the cache (see run()). The stamp has to freeze
    // with it, so it is latched on entry rather than recomputed from frameIdx.
    bool     cacheFrozen  = false;
    uint32_t frozenStamp  = 1;

    // ------------------ full-sized framebuffer : gbuffer → resolve → final --------------------
    glm::ivec2 displaySize;
    glm::ivec2 framebufferSize;
    glm::ivec2 framebufferPos;
    GLuint gbufferTex = 0; // RGBA32UI, full-res; written by primary, read by downscale/resolve
    GLuint resolveTex = 0; // RGBA32F,  full-res; sampled by final.frag
    void framebufferEvent();
    std::array<float, 24> genQuad(glm::vec2 size, glm::vec2 tex);


    // ------------------ GPU passes --------------------
    shader::Library     passes;
    shader::Defines     shaderDefines() const;
    shader::ComputePass primaryPass  {passes, "./shd/primary.comp",       shaderDefines(), shader::UBO_CAMERA};
    shader::ComputePass downscalePass{passes, "./shd/downscale.comp",     shaderDefines()};
    shader::ComputePass buildArgsPass{passes, "./shd/buildArgs.comp",     shaderDefines()};
    shader::ComputePass claimPass    {passes, "./shd/lbuffer_claim.comp", shaderDefines()};
    shader::ComputePass editMarkPass {passes, "./shd/edit_mark.comp",     shaderDefines()};
    shader::ComputePass normalPass   {passes, "./shd/normal.comp",        shaderDefines()};
    shader::ComputePass shadePass    {passes, "./shd/shade.comp",         shaderDefines(), shader::UBO_CAMERA};
    shader::ComputePass accumPass    {passes, "./shd/accum.comp",         shaderDefines()};
    shader::ComputePass avgPass      {passes, "./shd/avg.comp",           shaderDefines()};
    shader::ComputePass holefillPass {passes, "./shd/holefill.comp",      shaderDefines()};
    shader::ComputePass resolvePass  {passes, "./shd/resolve.comp",       shaderDefines(), shader::UBO_CAMERA};
    shader::FinalRasterPass finalPass{passes, "./shd/final.vert", "./shd/final.frag"};

    // ------------------ dispatch shapes --------------------
    shader::Dispatch fullRes()    const { return shader::Dispatch::direct(framebufferSize); }
    shader::Dispatch virtualRes() const { return shader::Dispatch::direct(virtualSize); }
    shader::Dispatch perVoxel()   const { return shader::Dispatch::indirect(indirectArgsSSBO); }
    shader::Dispatch single()     const { return shader::Dispatch::direct(glm::ivec2(1), glm::ivec2(1)); }

    // ------------------ virtual gbuffer : downscaled gbuffer --------------------
    GLuint     virtualGBufferTex   = 0; // RGBA32UI, virtual-res (downscaled gbuffer)
    GLuint     virtualDiffuseTex   = 0; // RGBA32F, virtual-res (incident diffuse E/π)
    GLuint     virtualSpecularTex  = 0; // RGBA32F, virtual-res (specular outgoing radiance)
    GLuint     holeFillTex         = 0; // RGBA32UI, virtual-res (x=diffuse, y=specular; RGB9E5 or IRR_HOLE)
    glm::ivec2 virtualSize         = glm::ivec2(0);
    int        curVirtualScale     = 0; // scale the virtual textures are currently sized for
    GLuint     jitterPeriod        = 1;  // S = scale²
    GLuint     jitterStride        = 1;  // coprime to scale
    void allocVirtualTextures();

    // ------------------ lbuffer : voxel cache --------------------
    static constexpr GLint      LBUFFER_SLOTS_TOTAL = 1 << 22;   // 4.19 M slots (keep in sync w/ lbuffer.glsl)
    static constexpr GLint      LBUFFER_SLOT_DWORDS = 16;
    static constexpr GLsizeiptr LBUFFER_BYTES = (GLsizeiptr)LBUFFER_SLOTS_TOTAL * LBUFFER_SLOT_DWORDS * (GLsizeiptr)sizeof(GLuint); // 256 MB
    GLuint lBufferSSBO = 0;

    // ------------------ per-voxel accumulators : uniqueList + accumulators --------------------
    GLuint accumDiffuseSSBO  = 0;   // uvec4[] : per-list-entry diffuse accumulator (xyz = RGB, w = pixelCount)
    GLuint accumSpecularSSBO = 0;   // uvec4[] : per-list-entry specular accumulator (xyz = RGB, w spare)
    GLuint uniqueListSSBO   = 0;   // uvec4[] : {key.xy, claimedSlot, spare}
    GLuint uniqueCountSSBO  = 0;   // uint
    GLuint indirectArgsSSBO = 0;   // uvec4[2]
    void allocVoxelLists();        // (re)size uniqueList + both accumulators to the virtual res

};
