#pragma once
#include "core.hpp"
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

    // Per-frame output exposed to the application / UI.
    Profiler         profiler;
    core::FrameStats stats;

    // rayPass.texture  = wavefront GBuffer color image (RGBA32UI).
    // finalPass.texture = resolved RGBA32F output read by the viewport widget.
    core::RasterPass rayPass;
    core::RasterPass finalPass;

    const char* glsl_version = "#version 430";

private:
    bool success = true;
    uint32_t frameID = 0;

    // Internal framebuffer / display geometry — not relevant outside Renderer.
    struct RuntimeMem {
        glm::ivec2 displaySize;
        glm::ivec2 framebufferSize;
        glm::ivec2 framebufferPos;
    } rrm;

    core::hashBuffer lBuffer;
    core::hashBuffer nBuffer;

    // normalPass = nBuffer normal refinement.
    // avgPass.texture = resolve.comp RGBA32F output (avg.comp program unused).
    core::ComputePass normalPass;
    core::ComputePass avgPass;

    // Wavefront compute passes (generate_primary → schedule → rank →
    // threshold → emit → trace → shade → resolve). thresholdPass is a tiny
    // single-workgroup scan that derives next-frame importance thresholds
    // from the histogram rank.comp populates — replaces the old CPU readback
    // of min/max stats.
    core::ComputePass primaryPass;
    core::ComputePass schedulePass;
    core::ComputePass rankPass;
    core::ComputePass thresholdPass;
    core::ComputePass emitPass;
    core::ComputePass tracePass;
    core::ComputePass shadePass;
    core::ComputePass resolvePass;

    // Wavefront SSBOs (bound once per frame at fixed binding points).
    GLuint rayRingBuf       = 0; // binding 0 — rays[RAY_CAPACITY]
    GLuint ringMetaBuf      = 0; // binding 1 — head/tail/pad/pad
    GLuint shadeListBuf     = 0; // binding 2 — shade_list[SHADE_BUDGET]
    GLuint shadeMetaBuf     = 0; // binding 3 — shade_count/pads
    GLuint emissiveListBuf  = 0; // binding 4 — emissives[EMISSIVE_CAPACITY]
    GLuint emissiveMetaBuf  = 0; // binding 5 — emissive_count/pads
    GLuint candidateListBuf = 0; // binding 6 — candidates[CANDIDATE_CAPACITY]
    GLuint candidateMetaBuf = 0; // binding 7 — candidate_count/pads
    GLuint rankStatsBuf     = 0; // binding 8 — thresholds + min/max/count + 256-bin histogram
                                 //              (1056 bytes; see wavefront.glsl::RankStats)

    // Wavefront images.
    GLuint claimMapTex       = 0; // r32ui CLAIM_WIDTH × CLAIM_PROBES
    GLuint sampleCountMapTex = 0; // r32ui CLAIM_WIDTH × CLAIM_PROBES
    GLuint emissiveClaimTex  = 0; // r32ui EMISSIVE_CLAIM_W × EMISSIVE_CLAIM_PROBES

    // Wavefront constants (must match #defines in wavefront.glsl).
    static constexpr GLint  LBUFFER_SLOTS         = 32u;
    static constexpr GLint  NBUFFER_SLOTS         = 32u;
    static constexpr GLuint RAY_CAPACITY_HOST     = 49152u;
    static constexpr GLuint SHADE_BUDGET_MAX      = 196608u;
    static constexpr GLint  CLAIM_WIDTH           = 16384;
    static constexpr GLint  CLAIM_PROBES          = 64;
    static constexpr GLuint EMISSIVE_CAPACITY     = 16384u;
    static constexpr GLint  EMISSIVE_CLAIM_W      = 8192;
    static constexpr GLint  EMISSIVE_CLAIM_PROBES = 8;
    static constexpr GLsizeiptr EMISSIVE_ENTRY_BYTES  = 32;
    static constexpr GLuint CANDIDATE_CAPACITY    = 1048576u;
    static constexpr GLsizeiptr CANDIDATE_ENTRY_BYTES = 16;

    // Importance thresholds now live entirely on the GPU inside rankStatsBuf
    // (see wavefront.glsl::RankStats). threshold.comp writes them at end of
    // frame N; rank.comp reads them at start of frame N+1. No CPU sync.

    core::RenderType currentRenderType = core::RenderType::DEFAULT;

    GLuint positionTexture;

    Octree       *volume;
    Camera       *camera;
    MaterialPool *materialPool;
    Skybox       *skybox;

    void framebufferEvent();
    void initWavefrontBuffers();
    void runWavefrontFrame(core::FrameConfig *frameConfig);
    void handleShaderRecompilation(core::FrameConfig *frameConfig);
    float* genQuad(glm::vec2 size, glm::vec2 tex);
    void linkCompute(core::ComputePass *pass, const char *shaderFile);
    void linkRaster (core::RasterPass  *pass, const char *vertexFile, const char *fragmentFile);
    GLuint compileShader(const char* path, std::string type, GLuint gl_type);
    void relinkCompute(core::ComputePass *pass, const char *shaderFile);
    void checkProgramCompileErrors(unsigned int shader);
    void checkGLError(const char* label, bool *success_s);
};
