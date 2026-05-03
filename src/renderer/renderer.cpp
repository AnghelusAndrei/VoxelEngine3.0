#include "renderer.hpp"
#include <stdio.h>
#include <string.h>

// Ray ring layout (must match wavefront.glsl::Ray sizeof). Bumped from 80 → 96
// when the ray gained separate direct_light / indirect_light vec3 fields for
// the dual-channel lBuffer EMA.
#define RAY_STRIDE    96
#define NBUFFER_STRIDE 3
// lBuffer stride (must match wavefront.glsl::lBufferStride). Bumped from 7 → 12
// to accommodate the direct/indirect channel split — see wavefront.glsl for the
// per-slot layout.
#define LBUFFER_STRIDE 12

// rankStatsBuf size on the GPU side (must match wavefront.glsl::RankStats).
//   thigh+tmid+pads (16) + min/max/count+pad (16) + histogram[256]*4 (1024).
#define RANK_STATS_BYTES 1056
#define RANK_STATS_HISTOGRAM_OFFSET 32

Renderer::Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_,
                   MaterialPool *materialPool_, Skybox *skybox_)
    : config(config_), volume(volume_), camera(camera_),
      materialPool(materialPool_), skybox(skybox_)
{
    printf("GL version: %s\n", glGetString(GL_VERSION));

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &lBuffer.size.x);
    lBuffer.slots  = LBUFFER_SLOTS;
    lBuffer.stride = LBUFFER_STRIDE;
    lBuffer.size.y = lBuffer.stride * lBuffer.slots;

    nBuffer.stride = NBUFFER_STRIDE;
    nBuffer.slots  = NBUFFER_SLOTS;
    nBuffer.size.x = lBuffer.size.x;
    nBuffer.size.y = nBuffer.stride * nBuffer.slots;

    if (config->debuggingEnabled)
        config->logMessage("[%f] initializing the renderer \n", glfwGetTime());
    checkGLError("Renderer initialization", &success);

    linkCompute(&normalPass,   "./shd/normal.comp");
    linkRaster (&finalPass,    "./shd/final.vert", "./shd/final.frag");
    linkCompute(&primaryPass,  "./shd/generate_primary.comp");
    linkCompute(&schedulePass,  "./shd/schedule.comp");
    linkCompute(&rankPass,      "./shd/rank.comp");
    linkCompute(&thresholdPass, "./shd/threshold.comp");
    linkCompute(&emitPass,      "./shd/emit.comp");
    linkCompute(&tracePass,     "./shd/trace.comp");
    linkCompute(&shadePass,     "./shd/shade.comp");
    linkCompute(&resolvePass,        "./shd/resolve.comp");

    if (config->debuggingEnabled)
        config->logMessage("[%f] compiled shaders \n", glfwGetTime());
    checkGLError("Shader linking", &success);

    camera      ->GenUBO(primaryPass.program);
    volume      ->GenUBO(primaryPass.program);
    materialPool->GenUBO(shadePass.program);
    skybox      ->GenUBO(primaryPass.program);

    auto bindCameraBlock = [](GLuint program) {
        GLuint idx = glGetUniformBlockIndex(program, "CameraUniform");
        if (idx != GL_INVALID_INDEX) glUniformBlockBinding(program, idx, 0);
    };
    bindCameraBlock(primaryPass.program);
    bindCameraBlock(emitPass.program);
    bindCameraBlock(resolvePass.program);

    auto bindMaterialBlock = [](GLuint program) {
        GLuint idx = glGetUniformBlockIndex(program, "MaterialUniform");
        if (idx != GL_INVALID_INDEX) glUniformBlockBinding(program, idx, 1);
    };
    bindMaterialBlock(shadePass.program);
    bindMaterialBlock(resolvePass.program);

    rrm.displaySize = config->framebufferSize();

    float *textureQuad = genQuad(glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 1.0f));

    glGenVertexArrays(1, &finalPass.VAO);
    glGenBuffers(1, &finalPass.VBO);
    glBindVertexArray(finalPass.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, finalPass.VBO);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), textureQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (config->debuggingEnabled)
        config->logMessage("[%f] generated Quad Vertex Array Objects \n", glfwGetTime());
    checkGLError("Quad objects initialization", &success);

    glGenTextures(1, &avgPass.texture);
    glGenTextures(1, &positionTexture);
    glGenTextures(1, &rayPass.texture);

    // finalPass FBO objects — must exist before the first framebufferEvent() call.
    glGenFramebuffers (1, &finalPass.framebuffer);
    glGenRenderbuffers(1, &finalPass.rbo);
    glGenTextures     (1, &finalPass.texture);

    if (config->debuggingEnabled)
        config->logMessage("[%f] building normal buffer \n", glfwGetTime());
    checkGLError("generated textures", &success);

    glGenTextures(1, &nBuffer.texture);
    glBindTexture(GL_TEXTURE_2D, nBuffer.texture);
    {
        std::vector<GLuint> zeros(size_t(nBuffer.size.x) * size_t(nBuffer.size.y), 0u);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, nBuffer.size.x, nBuffer.size.y, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (config->debuggingEnabled)
        config->logMessage("[%f] building lighting buffer \n", glfwGetTime());
    checkGLError("Generated normal buffer", &success);

    glGenTextures(1, &lBuffer.texture);
    glBindTexture(GL_TEXTURE_2D, lBuffer.texture);
    {
        std::vector<GLuint> zeros(size_t(lBuffer.size.x) * size_t(lBuffer.size.y), 0u);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, lBuffer.size.x, lBuffer.size.y, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (config->debuggingEnabled)
        config->logMessage("[%f] built lighting buffer \n", glfwGetTime());
    checkGLError("Generated lighting buffer", &success);

    framebufferEvent();
    initWavefrontBuffers();

    if (config->debuggingEnabled)
        config->logMessage("[%f] generated all framebuffer objects \n", glfwGetTime());
    checkGLError("Renderer initialization end", &success);
}

// -----------------------------------------------------------------------------
// Wavefront SSBO / image allocation (resolution-independent, called once).
// -----------------------------------------------------------------------------
void Renderer::initWavefrontBuffers() {
    const GLsizeiptr ringBytes = GLsizeiptr(RAY_CAPACITY_HOST) * RAY_STRIDE;
    glGenBuffers(1, &rayRingBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, rayRingBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, ringBytes, nullptr, GL_DYNAMIC_DRAW);

    GLuint zeros4[4] = {0, 0, 0, 0};

    glGenBuffers(1, &ringMetaBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ringMetaBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros4), zeros4, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &shadeListBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadeListBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 GLsizeiptr(SHADE_BUDGET_MAX) * GLsizeiptr(2 * sizeof(GLuint)),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &shadeMetaBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadeMetaBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros4), zeros4, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &emissiveListBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, emissiveListBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 GLsizeiptr(EMISSIVE_CAPACITY) * EMISSIVE_ENTRY_BYTES,
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &emissiveMetaBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, emissiveMetaBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros4), zeros4, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &candidateListBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, candidateListBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 GLsizeiptr(CANDIDATE_CAPACITY) * CANDIDATE_ENTRY_BYTES,
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &candidateMetaBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, candidateMetaBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros4), zeros4, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &rankStatsBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, rankStatsBuf);
    {
        // Allocate the full 1056-byte RankStats SSBO (thresholds + min/max +
        // histogram). Initialize the persistent threshold half so frame 0 sees
        // sentinel +INF / -INF and falls into the safe mid-tier-only path.
        std::vector<GLuint> init(RANK_STATS_BYTES / sizeof(GLuint), 0u);
        // Layout: [0]=thigh_bits, [1]=tmid_bits, then min/max/count, then histogram.
        const GLuint posInfBits = 0x7F800000u;   // +INF
        const GLuint negInfBits = 0xFF800000u;   // -INF
        init[0] = posInfBits;     // thigh_bits → never satisfied → no top tier
        init[1] = negInfBits;     // tmid_bits  → always satisfied → all mid tier
        init[4] = 0x7F7FFFFFu;    // min_imp_bits init for atomicMin
        init[5] = 0u;             // max_imp_bits init for atomicMax
        init[6] = 0u;             // stats_count
        // histogram[256] starts at uint index 8, all zero — fine.
        glBufferData(GL_SHADER_STORAGE_BUFFER, RANK_STATS_BYTES, init.data(), GL_DYNAMIC_DRAW);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    {
        std::vector<GLuint> zeros(size_t(CLAIM_WIDTH) * size_t(CLAIM_PROBES), 0u);

        glGenTextures(1, &claimMapTex);
        glBindTexture(GL_TEXTURE_2D, claimMapTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, CLAIM_WIDTH, CLAIM_PROBES, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenTextures(1, &sampleCountMapTex);
        glBindTexture(GL_TEXTURE_2D, sampleCountMapTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, CLAIM_WIDTH, CLAIM_PROBES, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    {
        std::vector<GLuint> zeros(size_t(EMISSIVE_CLAIM_W) * size_t(EMISSIVE_CLAIM_PROBES), 0u);
        glGenTextures(1, &emissiveClaimTex);
        glBindTexture(GL_TEXTURE_2D, emissiveClaimTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, EMISSIVE_CLAIM_W, EMISSIVE_CLAIM_PROBES, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    checkGLError("initWavefrontBuffers", &success);
}

// -----------------------------------------------------------------------------
// Wavefront frame dispatch.
// Sequence:
//   1. generate_primary.comp  — full-res primary rays → GBuffer
//   2. normal.comp            — refine normals from GBuffer hits (nBuffer path)
//   3. schedule.comp          — dedupe visible voxels → candidate list
//   4. rank.comp              — importance-aware sample budgeting → shade list
//   5. emit.comp × chunks     — shade list → ray ring (primary bounce)
//   6. (trace + shade)*       — bounce loop per chunk
//   7. resolve.comp           — GBuffer voxelID → lBuffer → RGBA32F output
// -----------------------------------------------------------------------------
void Renderer::runWavefrontFrame(core::FrameConfig *frameConfig) {
    GLuint glTime = (GLuint)(glfwGetTime() * 100.0);

    GLuint shadingBudget = (GLuint)frameConfig->shadingBudget;
    if (shadingBudget > SHADE_BUDGET_MAX) shadingBudget = SHADE_BUDGET_MAX;
    if (shadingBudget < 1u)              shadingBudget = 1u;

    GLuint frameSeed = (GLuint)(profiler.getResults() ? profiler.getResults()->CPU_ms : 0.0)
                       * 2654435761u + 1u;
    // Use a simple frame counter for the seed — the profiler doesn't expose
    // the raw frame index, so we derive a seed from time instead.
    frameSeed = (GLuint)(glfwGetTime() * 1000.0) * 2654435761u + 1u;

    // Bind GBuffer images up-front (reused by multiple stages).
    glBindImageTexture(0, rayPass.texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    glBindImageTexture(1, positionTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);

    // ---------------- 1. generate_primary.comp --------------------------------
    glUseProgram(primaryPass.program);
    {
        uint8_t unit = 0;
        volume->BindForAccumPass(primaryPass.program);
        unit = 1;
        skybox->BindUniforms(primaryPass.program, unit);
    }
    glUniform2i(glGetUniformLocation(primaryPass.program, "screenResolution"),
                rrm.framebufferSize.x, rrm.framebufferSize.y);
    glUniform1ui(glGetUniformLocation(primaryPass.program, "controlchecks"),
                 (GLuint)frameConfig->primary_controlchecks);

    profiler.primaryStart();
    glDispatchCompute((GLuint)ceil(rrm.framebufferSize.x / 8.0f),
                      (GLuint)ceil(rrm.framebufferSize.y / 8.0f), 1);
    profiler.primaryEnd();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    checkGLError("primaryPass", &success);

    // ---------------- 2. normal.comp (nBuffer-only path) ----------------------
    glUseProgram(normalPass.program);
    glBindImageTexture(0, rayPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, lBuffer.texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(2, positionTexture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R32UI);
    glBindImageTexture(3, nBuffer.texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    volume->BindForAccumPass(normalPass.program);
    glUniform1i(glGetUniformLocation(normalPass.program, "nBufferWidth"), nBuffer.size.x);
    glUniform1i(glGetUniformLocation(normalPass.program, "nBufferSlots"), nBuffer.slots);
    glUniform1i(glGetUniformLocation(normalPass.program, "lBufferSlots"), lBuffer.slots);
    glUniform1i(glGetUniformLocation(normalPass.program, "lBufferWidth"), lBuffer.size.x);
    glUniform1i(glGetUniformLocation(normalPass.program, "normalPrecision"), frameConfig->normalPrecision);
    glUniform2i(glGetUniformLocation(normalPass.program, "screenResolution"),
                rrm.framebufferSize.x, rrm.framebufferSize.y);
    glUniform1ui(glGetUniformLocation(normalPass.program, "time"),       glTime);
    glUniform1ui(glGetUniformLocation(normalPass.program, "updateTime"), frameConfig->updateTime);

    profiler.normalStart();
    glDispatchCompute((GLuint)ceil(rrm.framebufferSize.x / 8.0f),
                      (GLuint)ceil(rrm.framebufferSize.y / 8.0f), 1);
    profiler.normalEnd();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    checkGLError("normalPass", &success);

    // ---------------- Per-frame reset ----------------------------------------
    {
        GLuint zeros4[4] = {0, 0, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ringMetaBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros4), zeros4);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadeMetaBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros4), zeros4);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, candidateMetaBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros4), zeros4);

        // Reset min/max/count + histogram, BUT preserve thigh_bits/tmid_bits
        // which threshold.comp wrote at the end of the previous frame —
        // rank.comp reads them at the start of this frame.
        // Layout (uint indices): [0..3] thresholds (preserve), [4..7] min/max/count (reset),
        //                        [8..263] histogram (reset).
        GLuint statsReset[4] = {0x7F7FFFFFu, 0u, 0u, 0u};   // min=+INF bits, max=0, count=0, pad
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, rankStatsBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 16, sizeof(statsReset), statsReset);
        // Histogram starts at byte 32. 256 bins × 4 = 1024 bytes.
        std::vector<GLuint> histoZeros(256, 0u);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, RANK_STATS_HISTOGRAM_OFFSET,
                        histoZeros.size() * sizeof(GLuint), histoZeros.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        std::vector<GLuint> claimZeros(size_t(CLAIM_WIDTH) * size_t(CLAIM_PROBES), 0u);
        glBindTexture(GL_TEXTURE_2D, claimMapTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CLAIM_WIDTH, CLAIM_PROBES,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, claimZeros.data());
        glBindTexture(GL_TEXTURE_2D, sampleCountMapTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CLAIM_WIDTH, CLAIM_PROBES,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, claimZeros.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Bind SSBOs for the remaining passes.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, rayRingBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ringMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, shadeListBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, shadeMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, emissiveListBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, emissiveMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, candidateListBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, candidateMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, rankStatsBuf);

    // ---------------- 3. schedule.comp ----------------------------------------
    glUseProgram(schedulePass.program);
    glBindImageTexture(0, rayPass.texture,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, claimMapTex,       0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(2, sampleCountMapTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glUniform2i(glGetUniformLocation(schedulePass.program, "screenResolution"),
                rrm.framebufferSize.x, rrm.framebufferSize.y);
    glUniform1i (glGetUniformLocation(schedulePass.program, "claimWidth"),  CLAIM_WIDTH);
    glUniform1i (glGetUniformLocation(schedulePass.program, "claimProbes"), CLAIM_PROBES);
    glUniform1ui(glGetUniformLocation(schedulePass.program, "frameSeed"),   frameSeed);

    profiler.scheduleStart();
    glDispatchCompute((GLuint)ceil(rrm.framebufferSize.x / 8.0f),
                      (GLuint)ceil(rrm.framebufferSize.y / 8.0f), 1);
    profiler.scheduleEnd();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    checkGLError("schedulePass", &success);

    // Sync-readback of candidate count — unavoidable; single uint.
    GLuint candidateCount = 0;
    {
        double t0 = glfwGetTime() * 1000.0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, candidateMetaBuf);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &candidateCount);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        profiler.setScheduleReadback(glfwGetTime() * 1000.0 - t0);
    }
    if (candidateCount > CANDIDATE_CAPACITY) candidateCount = CANDIDATE_CAPACITY;

    // ---------------- 3b. rank.comp + threshold.comp -------------------------
    // IMPORTANT: profiler.{rank,threshold}{Start,End} are called
    // UNCONDITIONALLY around their dispatches. The 5-slot timestamp ring in
    // doReadback reads each query 3 frames later regardless of whether the
    // dispatch actually ran. A skipped pair of stamps leaves the query
    // "unset" and the readback then returns GL_INVALID_OPERATION (1282) —
    // which surfaces on the NEXT checkGLError call (typically primaryPass
    // a few frames later, far from the actual cause).
    GLuint scheduledCount = 0;

    profiler.rankStart();
    if (candidateCount > 0u) {
        glUseProgram(rankPass.program);
        glBindImageTexture(0, lBuffer.texture,   0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
        glBindImageTexture(1, sampleCountMapTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
        glUniform1i (glGetUniformLocation(rankPass.program, "lBufferWidth"), lBuffer.size.x);
        glUniform1i (glGetUniformLocation(rankPass.program, "lBufferSlots"), lBuffer.slots);
        glUniform1f (glGetUniformLocation(rankPass.program, "wPixels"),      frameConfig->weightPixels);
        glUniform1f (glGetUniformLocation(rankPass.program, "wVariance"),    frameConfig->weightVariance);
        glUniform1f (glGetUniformLocation(rankPass.program, "pixelsNorm"),   frameConfig->pixelsNorm);
        glUniform1f (glGetUniformLocation(rankPass.program, "varianceNorm"), frameConfig->varianceNorm);
        // tHigh / tMid are no longer uniforms — rank.comp reads them from the
        // RankStats SSBO (binding 8), where threshold.comp wrote them at the
        // end of the previous frame. See wavefront.glsl::RankStats.
        glUniform1i (glGetUniformLocation(rankPass.program, "minSamples"),   frameConfig->minSamples);
        glUniform1ui(glGetUniformLocation(rankPass.program, "shadeBudget"),  shadingBudget);
        glDispatchCompute((candidateCount + 63u) / 64u, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        checkGLError("rankPass", &success);
    }
    profiler.rankEnd();

    // threshold.comp: tiny single-workgroup scan over rank.comp's histogram.
    // Dispatched immediately so it pipelines with the shadeCount readback below.
    profiler.thresholdStart();
    if (candidateCount > 0u) {
        glUseProgram(thresholdPass.program);
        glUniform1f(glGetUniformLocation(thresholdPass.program, "topFraction"),
                    frameConfig->topFraction);
        glUniform1f(glGetUniformLocation(thresholdPass.program, "midFraction"),
                    frameConfig->midFraction);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        checkGLError("thresholdPass", &success);
    }
    profiler.thresholdEnd();

    if (candidateCount > 0u) {
        double t0 = glfwGetTime() * 1000.0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadeMetaBuf);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &scheduledCount);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        profiler.setRankReadback(glfwGetTime() * 1000.0 - t0);
    }

    stats.scheduledCount = scheduledCount;
    stats.shadingBudget  = shadingBudget;
    if (scheduledCount > shadingBudget) scheduledCount = shadingBudget;

    // ---------------- 4 + 5: chunked emit / trace / shade loop ---------------
    int numChunks = scheduledCount / RAY_CAPACITY_HOST
                  + ((scheduledCount % RAY_CAPACITY_HOST) ? 1 : 0);
    profiler.setNumChunks(numChunks);
    profiler.chunkLoopStart();

    for (GLuint chunkBase = 0; chunkBase < scheduledCount; chunkBase += RAY_CAPACITY_HOST) {
        int      chunkIdx  = (int)(chunkBase / RAY_CAPACITY_HOST);
        GLuint   chunkSize = (scheduledCount - chunkBase < RAY_CAPACITY_HOST)
                           ? (scheduledCount - chunkBase) : RAY_CAPACITY_HOST;

        // Reset ring meta for this chunk.
        {
            GLuint zeros4[4] = {0, 0, 0, 0};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ringMetaBuf);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros4), zeros4);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }

        // --- emit ---
        glUseProgram(emitPass.program);
        glBindImageTexture(0, rayPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
        glUniform2i(glGetUniformLocation(emitPass.program, "screenResolution"),
                    rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1ui(glGetUniformLocation(emitPass.program, "chunkBase"), chunkBase);
        glUniform1ui(glGetUniformLocation(emitPass.program, "chunkSize"), chunkSize);
        glUniform1ui(glGetUniformLocation(emitPass.program, "frameSeed"), frameSeed);
        glUniform1ui(glGetUniformLocation(emitPass.program, "frameIdx"),  (GLuint)frameID);
        glUniform1ui(glGetUniformLocation(emitPass.program, "octreeDepth_"), (GLuint)volume->depth);

        profiler.emitStart(chunkIdx);
        glDispatchCompute((chunkSize + 63u) / 64u, 1, 1);
        profiler.emitEnd(chunkIdx);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        checkGLError("emitPass", &success);

        // --- bounce loop ---
        profiler.bouncesStart(chunkIdx);
        int numBounces = 0;
        for (GLuint bounce = 0; bounce < 8u; bounce++) {
            GLuint meta[2] = {0, 0};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ringMetaBuf);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(meta), meta);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            GLuint rayHead = meta[0], rayTail = meta[1];
            if (rayHead == rayTail) break;
            GLuint windowSize = rayHead - rayTail;

            // --- trace ---
            glUseProgram(tracePass.program);
            volume->BindForAccumPass(tracePass.program);
            glUniform1ui(glGetUniformLocation(tracePass.program, "controlchecks"),
                         (GLuint)frameConfig->bounce_controlchecks);
            glUniform1ui(glGetUniformLocation(tracePass.program, "rayWindowBase"), rayTail);
            glUniform1ui(glGetUniformLocation(tracePass.program, "rayWindowSize"), windowSize);

            profiler.traceStart(chunkIdx, (int)bounce);
            glDispatchCompute((windowSize + 63u) / 64u, 1, 1);
            profiler.traceEnd(chunkIdx, (int)bounce);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            checkGLError("tracePass", &success);

            // --- shade ---
            glUseProgram(shadePass.program);
            glBindImageTexture(0, lBuffer.texture,  0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            glBindImageTexture(1, nBuffer.texture,  0, GL_FALSE, 0, GL_READ_ONLY,  GL_R32UI);
            glBindImageTexture(2, emissiveClaimTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            {
                uint8_t unit = 0;
                volume->BindForAccumPass(shadePass.program);
                unit = 1;
                skybox->BindUniforms(shadePass.program, unit);
            }
            glUniform1ui(glGetUniformLocation(shadePass.program, "controlchecks"),
                         (GLuint)frameConfig->bounce_controlchecks);
            glUniform1ui(glGetUniformLocation(shadePass.program, "rayWindowBase"), rayTail);
            glUniform1ui(glGetUniformLocation(shadePass.program, "rayWindowSize"), windowSize);
            glUniform1i (glGetUniformLocation(shadePass.program, "lBufferWidth"), lBuffer.size.x);
            glUniform1i (glGetUniformLocation(shadePass.program, "lBufferSlots"), lBuffer.slots);
            glUniform1i (glGetUniformLocation(shadePass.program, "nBufferWidth"), nBuffer.size.x);
            glUniform1i (glGetUniformLocation(shadePass.program, "nBufferSlots"), nBuffer.slots);
            glUniform1ui(glGetUniformLocation(shadePass.program, "time"),         glTime);
            glUniform1ui(glGetUniformLocation(shadePass.program, "updateTime"),   frameConfig->updateTime);
            glUniform1ui(glGetUniformLocation(shadePass.program, "SAMPLE_CAP_DIRECT"),
                         (GLuint)frameConfig->sampleCapDirect);
            glUniform1ui(glGetUniformLocation(shadePass.program, "SAMPLE_CAP_INDIRECT"),
                         (GLuint)frameConfig->sampleCapIndirect);
            glUniform1f (glGetUniformLocation(shadePass.program, "fireflyK"),
                         frameConfig->fireflyK);
            glUniform1f (glGetUniformLocation(shadePass.program, "fireflyFloor"),
                         frameConfig->fireflyFloor);
            glUniform1i (glGetUniformLocation(shadePass.program, "disableStaleReset"),
                         frameConfig->disableStaleReset);

            profiler.shadeStart(chunkIdx, (int)bounce);
            glDispatchCompute((windowSize + 63u) / 64u, 1, 1);
            profiler.shadeEnd(chunkIdx, (int)bounce);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            checkGLError("shadePass", &success);

            // Advance tail to old head — survivors are appended past old head.
            GLuint newTail = rayHead;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ringMetaBuf);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), sizeof(GLuint), &newTail);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            numBounces = (int)bounce + 1;
        }
        profiler.bouncesEnd(chunkIdx, numBounces);
    }

    profiler.chunkLoopEnd();

    // tHigh / tMid for the next frame were already derived on-GPU by
    // threshold.comp (dispatched above, immediately after rank.comp). No
    // host-side update needed — the values live entirely inside RankStats.

    // ---------------- 6. resolve.comp ----------------------------------------
    // resolve.comp does the per-pixel lBuffer lookup and writes raw radiance
    // into avgPass.texture. 
    glUseProgram(resolvePass.program);
    glBindImageTexture(0, lBuffer.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R32UI);
    glBindImageTexture(1, rayPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(2, avgPass.texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(3, lBuffer.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_R32UI);
    {
        uint8_t unit = 0;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, nBuffer.texture);
        glUniform1i(glGetUniformLocation(resolvePass.program, "nBuffer"), (int)unit);
        unit++;
        skybox->BindUniforms(resolvePass.program, unit);
    }
    glUniform1i (glGetUniformLocation(resolvePass.program, "nBufferWidth"), nBuffer.size.x);
    glUniform1i (glGetUniformLocation(resolvePass.program, "nBufferSlots"), nBuffer.slots);
    glUniform1i (glGetUniformLocation(resolvePass.program, "lBufferWidth"), lBuffer.size.x);
    glUniform1i (glGetUniformLocation(resolvePass.program, "lBufferSlots"), lBuffer.slots);
    glUniform2i (glGetUniformLocation(resolvePass.program, "screenResolution"),
                 rrm.framebufferSize.x, rrm.framebufferSize.y);
    glUniform1i (glGetUniformLocation(resolvePass.program, "renderType"),
                 (GLint)frameConfig->renderType);
    glUniform1ui(glGetUniformLocation(resolvePass.program, "primary_controlchecks"),
                 (GLuint)frameConfig->primary_controlchecks);
    glUniform1i (glGetUniformLocation(resolvePass.program, "reconstructionRadius"),
                 (GLint)frameConfig->reconstructionRadius);
    glUniform1f (glGetUniformLocation(resolvePass.program, "varianceNormForView"),
                 frameConfig->varianceNorm);

    profiler.resolveStart();
    glDispatchCompute((GLuint)ceil(rrm.framebufferSize.x / 8.0f),
                      (GLuint)ceil(rrm.framebufferSize.y / 8.0f), 1);
    profiler.resolveEnd();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    checkGLError("resolvePass", &success);
}

// -----------------------------------------------------------------------------
// run() — one full frame: readback → setup → wavefront → final blit.
// -----------------------------------------------------------------------------
bool Renderer::run(core::FrameConfig *frameConfig) {
    profiler.beginFrame(glfwGetTime() * 1000.0);

    // Populate FrameStats (written every frame so the UI always has fresh data).
    stats.scene_capacity = volume->capacity * sizeof(Octree::Node);
    stats.scene_mem      = volume->size     * sizeof(Octree::Node);
    stats.lBuffer_mem    = lBuffer.size.x * lBuffer.size.y * sizeof(GLuint);
    stats.nBuffer_mem    = nBuffer.size.x * nBuffer.size.y * sizeof(GLuint);
    stats.rayRing_mem    = GLsizeiptr(RAY_CAPACITY_HOST) * RAY_STRIDE;
    stats.shadeList_mem  = GLsizeiptr(SHADE_BUDGET_MAX)  * GLsizeiptr(2 * sizeof(GLuint));
    stats.claimMap_mem   = CLAIM_WIDTH * CLAIM_PROBES * sizeof(uint32_t);
    stats.voxels_num     = volume->numVoxels;
    stats.cam_position   = camera->position;
    stats.cam_direction  = camera->direction;

    handleShaderRecompilation(frameConfig);

    glm::ivec2 displayComparison = rrm.displaySize;
    rrm.displaySize = config->framebufferSize();
    if (rrm.displaySize != displayComparison) {
        framebufferEvent();
        if (config->debuggingEnabled)
            config->logMessage("[%f] framebuffer resized \n", glfwGetTime());
        checkGLError("Framebuffer resized", &success);
    }

    runWavefrontFrame(frameConfig);

    // Final blit — resolve output → default framebuffer (or renderToTexture FBO).
    glViewport(rrm.framebufferPos.x, rrm.framebufferPos.y,
               rrm.framebufferSize.x, rrm.framebufferSize.y);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (frameConfig->renderToTexture)
        glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(finalPass.program);
    glUniform2i(glGetUniformLocation(finalPass.program, "screenResolution"),
                rrm.framebufferSize.x, rrm.framebufferSize.y);
    glBindVertexArray(finalPass.VAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, avgPass.texture);
    glUniform1i(glGetUniformLocation(finalPass.program, "screenTexture"), 0);

    profiler.finalBlitStart();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    profiler.finalBlitEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("final pass", &success);

    profiler.endFrame(glfwGetTime() * 1000.0);
    frameID++;

    if (config->debuggingEnabled)
        config->logMessage("[%f] frame end \n", glfwGetTime());
    return success;
}

// -----------------------------------------------------------------------------
// Destructor
// -----------------------------------------------------------------------------
Renderer::~Renderer() {
    camera->freeVRAM();
    volume->freeVRAM();
    materialPool->freeVRAM();

    glDeleteVertexArrays(1, &finalPass.VAO);
    glDeleteBuffers(1, &finalPass.VBO);

    glDeleteTextures(1, &rayPass.texture);
    glDeleteTextures(1, &positionTexture);
    glDeleteTextures(1, &lBuffer.texture);
    glDeleteTextures(1, &nBuffer.texture);
    glDeleteTextures(1, &avgPass.texture);
    glDeleteTextures(1, &claimMapTex);
    glDeleteTextures(1, &sampleCountMapTex);
    glDeleteFramebuffers(1, &finalPass.framebuffer);
    glDeleteRenderbuffers(1, &finalPass.rbo);
    glDeleteTextures(1, &finalPass.texture);

    glDeleteBuffers(1, &rayRingBuf);
    glDeleteBuffers(1, &ringMetaBuf);
    glDeleteBuffers(1, &shadeListBuf);
    glDeleteBuffers(1, &shadeMetaBuf);
    glDeleteBuffers(1, &emissiveListBuf);
    glDeleteBuffers(1, &emissiveMetaBuf);
    glDeleteBuffers(1, &candidateListBuf);
    glDeleteBuffers(1, &candidateMetaBuf);
    glDeleteBuffers(1, &rankStatsBuf);
    glDeleteTextures(1, &emissiveClaimTex);

    glDeleteProgram(normalPass.program);
    glDeleteProgram(finalPass.program);
    glDeleteProgram(primaryPass.program);
    glDeleteProgram(schedulePass.program);
    glDeleteProgram(rankPass.program);
    glDeleteProgram(thresholdPass.program);
    glDeleteProgram(emitPass.program);
    glDeleteProgram(tracePass.program);
    glDeleteProgram(shadePass.program);
    glDeleteProgram(resolvePass.program);
    // profiler destructor handles its own GL query cleanup.
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
float* Renderer::genQuad(glm::vec2 size, glm::vec2 tex) {
    float *quadVertices = new float[24]{
        -size.x,  size.y,  0.0f, tex.y,
        -size.x, -size.y,  0.0f,  0.0f,
         size.x, -size.y,  tex.x, 0.0f,
        -size.x,  size.y,  0.0f,  tex.y,
         size.x, -size.y,  tex.x, 0.0f,
         size.x,  size.y,  tex.x, tex.y
    };
    return quadVertices;
}

void Renderer::framebufferEvent() {
    float windowAspect = (float)rrm.displaySize.x / (float)rrm.displaySize.y;

    if (windowAspect > config->aspectRatio) {
        rrm.framebufferSize.y = rrm.displaySize.y;
        rrm.framebufferSize.x = static_cast<int>(rrm.displaySize.y * config->aspectRatio);
        rrm.framebufferPos.x  = (rrm.displaySize.x - rrm.framebufferSize.x) / 2;
        rrm.framebufferPos.y  = 0;
    } else {
        rrm.framebufferSize.x = rrm.displaySize.x;
        rrm.framebufferSize.y = static_cast<int>(rrm.displaySize.x / config->aspectRatio);
        rrm.framebufferPos.x  = 0;
        rrm.framebufferPos.y  = (rrm.displaySize.y - rrm.framebufferSize.y) / 2;
    }

    normalPass.globalSize = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    normalPass.groupSize  = glm::ivec2(8, 8);
    avgPass.globalSize    = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    avgPass.groupSize     = glm::ivec2(8, 8);

    glBindTexture(GL_TEXTURE_2D, rayPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindTexture(GL_TEXTURE_2D, positionTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glBindTexture(GL_TEXTURE_2D, finalPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, finalPass.texture, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, finalPass.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          rrm.framebufferSize.x, rrm.framebufferSize.y);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, finalPass.rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        config->logMessage("RENDERER::ERROR::FRAMEBUFFER:: Framebuffer is not complete!\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glBindTexture(GL_TEXTURE_2D, avgPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::handleShaderRecompilation(core::FrameConfig *frameConfig) {
    if (currentRenderType != frameConfig->renderType) {
        currentRenderType = frameConfig->renderType;
        if (config->debuggingEnabled)
            config->logMessage("[%f] renderType changed to %d \n",
                               glfwGetTime(), (int)currentRenderType);
    }

    if (frameConfig->shaderRecompilation) {
        relinkCompute(&normalPass,   "./shd/normal.comp");
        relinkCompute(&primaryPass,  "./shd/generate_primary.comp");
        relinkCompute(&schedulePass,  "./shd/schedule.comp");
        relinkCompute(&rankPass,      "./shd/rank.comp");
        relinkCompute(&thresholdPass, "./shd/threshold.comp");
        relinkCompute(&emitPass,      "./shd/emit.comp");
        relinkCompute(&tracePass,     "./shd/trace.comp");
        relinkCompute(&shadePass,     "./shd/shade.comp");
        relinkCompute(&resolvePass,        "./shd/resolve.comp");

        auto bindCameraBlock = [](GLuint program) {
            GLuint idx = glGetUniformBlockIndex(program, "CameraUniform");
            if (idx != GL_INVALID_INDEX) glUniformBlockBinding(program, idx, 0);
        };
        bindCameraBlock(primaryPass.program);
        bindCameraBlock(emitPass.program);
        bindCameraBlock(resolvePass.program);

        auto bindMaterialBlock = [](GLuint program) {
            GLuint idx = glGetUniformBlockIndex(program, "MaterialUniform");
            if (idx != GL_INVALID_INDEX) glUniformBlockBinding(program, idx, 1);
        };
        bindMaterialBlock(shadePass.program);
        bindMaterialBlock(resolvePass.program);

        frameConfig->shaderRecompilation = false;
        if (config->debuggingEnabled)
            config->logMessage("[%f] recompiled wavefront shaders \n", glfwGetTime());
        checkGLError("shader recompilation", &success);
    }
}

void Renderer::linkCompute(core::ComputePass *pass, const char *shaderFile) {
    pass->shader  = compileShader(shaderFile, "COMPUTE", GL_COMPUTE_SHADER);
    pass->program = glCreateProgram();
    glAttachShader(pass->program, pass->shader);
    glLinkProgram(pass->program);
    glDeleteShader(pass->shader);
    checkProgramCompileErrors(pass->program);
}

void Renderer::linkRaster(core::RasterPass *pass, const char *vertexFile, const char *fragmentFile) {
    pass->vertexShader   = compileShader(vertexFile,   "VERTEX",   GL_VERTEX_SHADER);
    pass->fragmentShader = compileShader(fragmentFile, "FRAGMENT", GL_FRAGMENT_SHADER);
    pass->program        = glCreateProgram();
    glAttachShader(pass->program, pass->vertexShader);
    glAttachShader(pass->program, pass->fragmentShader);
    glLinkProgram(pass->program);
    glDeleteShader(pass->vertexShader);
    glDeleteShader(pass->fragmentShader);
    checkProgramCompileErrors(pass->program);
}

GLuint Renderer::compileShader(const char* path, std::string type, GLuint gl_type) {
    std::string shaderCodeRaw;
    std::ifstream shaderFile;
    shaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    try {
        shaderFile.open(path);
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        shaderFile.close();
        shaderCodeRaw = shaderStream.str();
    } catch (std::ifstream::failure& e) {
        if (config->debuggingEnabled)
            config->logMessage("[%f] RENDERER::ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ: %s \n",
                               glfwGetTime(), e.what());
    }

    // Resolve #include "filename" — one level deep, relative to shader directory.
    {
        std::string shaderDir = std::string(path);
        size_t slash = shaderDir.rfind('/');
        shaderDir = (slash != std::string::npos) ? shaderDir.substr(0, slash + 1) : "./";

        const std::string tag = "#include \"";
        size_t pos = 0;
        while ((pos = shaderCodeRaw.find(tag, pos)) != std::string::npos) {
            size_t nameStart = pos + tag.size();
            size_t nameEnd   = shaderCodeRaw.find('"', nameStart);
            if (nameEnd == std::string::npos) break;

            std::string includeName = shaderCodeRaw.substr(nameStart, nameEnd - nameStart);
            std::string includePath = shaderDir + includeName;

            std::string includeContent;
            std::ifstream incFile(includePath);
            if (incFile.good()) {
                std::stringstream ss;
                ss << incFile.rdbuf();
                includeContent = ss.str();
            } else if (config->debuggingEnabled) {
                config->logMessage("[%f] RENDERER::ERROR::SHADER::INCLUDE_NOT_FOUND: %s \n",
                                   glfwGetTime(), includePath.c_str());
            }
            shaderCodeRaw.replace(pos, nameEnd + 1 - pos, includeContent);
            pos += includeContent.size();
        }
    }

    const char* shaderCode = shaderCodeRaw.c_str();
    GLuint shader = glCreateShader(gl_type);
    glShaderSource(shader, 1, &shaderCode, NULL);
    glCompileShader(shader);

    int success_s;
    char infoLog[1024];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success_s);
    if (success_s) return shader;
    glGetShaderInfoLog(shader, 1024, NULL, infoLog);
    config->logMessage("[%f] RENDERER::SHADER_COMPILATION_ERROR\n %s \n", glfwGetTime(), infoLog);
    return shader;
}

void Renderer::relinkCompute(core::ComputePass *pass, const char *shaderFile) {
    pass->shader = compileShader(shaderFile, "COMPUTE", GL_COMPUTE_SHADER);
    GLuint newProgram = glCreateProgram();
    glAttachShader(newProgram, pass->shader);
    glLinkProgram(newProgram);
    checkProgramCompileErrors(newProgram);
    glDeleteProgram(pass->program);
    pass->program = newProgram;
}

void Renderer::checkProgramCompileErrors(unsigned int program) {
    int success;
    char infoLog[1024];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success) return;
    glGetProgramInfoLog(program, 1024, NULL, infoLog);
    config->logMessage("[%f] RENDERER::PROGRAM_LINKING_ERROR \n %s \n", glfwGetTime(), infoLog);
}

void Renderer::checkGLError(const char* label, bool *success_s) {
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        config->logMessage("[%f] GL_ERROR at %s: %u\n",
                           glfwGetTime(), label, (unsigned int)error);
        *success_s = false;
    }
}
