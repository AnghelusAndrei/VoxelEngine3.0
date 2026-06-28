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

    core::RasterPass finalPass;

    const char* glsl_version = "#version 430";

private:
    bool     success  = true;
    uint32_t frameIdx = 0;

    struct RuntimeMem {
        glm::ivec2 displaySize;
        glm::ivec2 framebufferSize;
        glm::ivec2 framebufferPos;
    } rrm;

    core::ComputePass primaryPass;   // DDA → gbuffer (RGBA32UI, primaryPass.texture)
    core::ComputePass resolvePass;   // gbuffer → resolveTex by render mode
    core::ComputePass buildArgsPass; // uniqueVoxelCount → indirectArgs (1 thread)
    core::ComputePass claimPass;     // lbuffer_claim (Increment 2d)
    core::ComputePass normalPass;    // normal.comp (Increment 3)
    core::ComputePass editMarkPass;  // edit_mark.comp (Increment 3)
    core::ComputePass restirPass;        // restir.comp (E1): per-voxel ReSTIR DI temporal reservoir
    core::ComputePass restirSpatialPass; // restir_spatial.comp (E1.1): spatial Z-combine → spatiotemporal loop
    core::ComputePass downscalePass; // downscale.comp (B1.1): gbuffer → virtual gbuffer
    core::ComputePass shadePass;     // shade.comp (B1.2): per-virtual-pixel path tracer
    core::ComputePass accumPass;     // accum.comp (B1.3): scatter virtual radiance → slots
    core::ComputePass avgPass;       // avg.comp   (B1.3): per-voxel temporal EMA blend
    core::ComputePass atrousPass;    // atrous.comp: adaptive specular-only edge-stop filter (optional)
    GLuint            resolveTex = 0; // RGBA32F, full-res; sampled by final.frag

    // Virtual framebuffer (B1.1/B1.2): the downscaled shading domain. Sized to
    // ceil(framebufferSize / virtualScale); reallocated on framebuffer / scale change.
    GLuint     virtualGBufferTex   = 0; // RGBA32UI, virtual-res (downscaled gbuffer)
    GLuint     virtualDiffuseTex   = 0; // RGBA32F, virtual-res (incident diffuse E/π)
    GLuint     virtualSpecularTex  = 0; // RGBA32F, virtual-res (specular outgoing radiance)
    GLuint     virtualNormalTex    = 0; // RGBA32F, virtual-res (xyz=normal, w=roughness; à-trous edge-stop)
    GLuint     virtualSpecularTex2 = 0; // RGBA32F, virtual-res (specular à-trous ping-pong scratch)
    glm::ivec2 virtualSize         = glm::ivec2(0);
    int        curVirtualScale     = 0; // scale the virtual textures are currently sized for
    void allocVirtualTextures();

    // lBuffer — persistent per-voxel radiance cache (see architecture/LBUFFER.md +
    // CLAIM.md). FLAT, lock-free, open-addressed: N power-of-two 16-DWORD slots
    // (64 B = 1 cache line); slot = hash(key)&(N-1), linear probe, claim via CAS on the
    // timestamp word. No buckets, no lock buffer, no retry buffers.
    static constexpr GLint      LBUFFER_SLOTS_TOTAL = 1 << 22;   // 4.19 M slots (keep in sync w/ lbuffer.glsl)
    static constexpr GLint      LBUFFER_SLOT_DWORDS = 16;
    static constexpr GLsizeiptr LBUFFER_BYTES =
        (GLsizeiptr)LBUFFER_SLOTS_TOTAL * LBUFFER_SLOT_DWORDS * (GLsizeiptr)sizeof(GLuint); // 256 MB
    static constexpr GLuint     NO_SLOT = 0xFFFFFFFFu;
    GLuint lBufferSSBO = 0;

    // ReSTIR DI spatiotemporal reservoir — a parallel buffer (binding 6, 2 DWORDs/slot). Spatial pass
    // writes here; shade + next-frame temporal read it → the feedback loop. See architecture/RESTIR.md §E1.
    static constexpr GLsizeiptr RESV_SPATIAL_BYTES =
        (GLsizeiptr)LBUFFER_SLOTS_TOTAL * 2 * (GLsizeiptr)sizeof(GLuint);   // 32 MB
    GLuint resvSpatialSSBO = 0;

    // Per-frame dedup pipeline buffers. uniqueList is sized to the framebuffer pixel
    // count (worst case: every pixel a distinct voxel) and reallocated in framebufferEvent().
    GLuint uniqueListSSBO   = 0;   // uvec4[] : {key.xy, claimedSlot, spare}
    GLuint uniqueCountSSBO  = 0;   // uint
    GLuint indirectArgsSSBO = 0;   // uvec4[2]
    void allocVoxelLists();        // (re)size uniqueList to the framebuffer

    Octree       *volume;
    Camera       *camera;
    MaterialPool *materialPool;
    Skybox       *skybox;

    void framebufferEvent();
    void handleShaderRecompilation(core::FrameConfig *frameConfig);
    std::array<float, 24> genQuad(glm::vec2 size, glm::vec2 tex);
};
