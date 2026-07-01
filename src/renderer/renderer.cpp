#include "renderer.hpp"

Renderer::Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_,
                   MaterialPool *materialPool_, Skybox *skybox_) :
                   config(config_), volume(volume_), camera(camera_),
                   materialPool(materialPool_), skybox(skybox_)
{
    config->logMessage("GL version: %s\n", glGetString(GL_VERSION));

    if (config->debuggingEnabled)
        config->logMessage("[%f] initializing the renderer\n", glfwGetTime());
    shader::checkGLError("Renderer initialization", success, *config);

    primaryPass.globalSize = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    primaryPass.groupSize  = glm::ivec2(8, 8);

    shader::linkCompute(primaryPass,   "./shd/primary.comp",       *config);
    shader::linkCompute(resolvePass,   "./shd/resolve.comp",       *config);
    shader::linkCompute(buildArgsPass, "./shd/buildArgs.comp",     *config);
    shader::linkCompute(claimPass,     "./shd/lbuffer_claim.comp", *config);
    shader::linkCompute(normalPass,    "./shd/normal.comp",        *config);
    shader::linkCompute(editMarkPass,  "./shd/edit_mark.comp",     *config);
    shader::linkCompute(downscalePass, "./shd/downscale.comp",     *config);
    shader::linkCompute(shadePass,     "./shd/shade.comp",         *config);
    shader::linkCompute(accumPass,     "./shd/accum.comp",         *config);
    shader::linkCompute(avgPass,       "./shd/avg.comp",           *config);
    shader::linkCompute(atrousPass,    "./shd/atrous.comp",        *config);
    shader::linkRaster (finalPass,     "./shd/final.vert", "./shd/final.frag", *config);

    // primary has only CameraUniform; resolve + shade have both Camera + Material UBOs.
    camera      ->setProgram(primaryPass.program);
    camera      ->setProgram(resolvePass.program);
    camera      ->setProgram(shadePass.program);
    materialPool->setProgram(resolvePass.program);
    materialPool->setProgram(shadePass.program);

    if (config->debuggingEnabled)
        config->logMessage("[%f] compiled shaders\n", glfwGetTime());
    shader::checkGLError("Shader linking", success, *config);

    camera      ->GenUBO();
    volume      ->GenUBO();
    materialPool->GenUBO();

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, camera->gl_ID);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, materialPool->gl_ID);

    // lBuffer radiance cache (SSBO). Zero-initialised, bound persistently. Flat,
    // open-addressed, 16-DWORD slots; claimed lock-free via CAS on the timestamp word
    // (see lbuffer.glsl / architecture/CLAIM.md). Zero = pristine-empty.
    glGenBuffers(1, &lBufferSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, lBufferSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, LBUFFER_BYTES, NULL, GL_DYNAMIC_DRAW);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_LBUFFER_BINDING, lBufferSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    shader::checkGLError("lBuffer SSBO", success, *config);

    // Dedup pipeline buffers. uniqueList is sized in allocVoxelLists() (via
    // framebufferEvent); the small count / args are fixed-size and allocated here.
    glGenBuffers(1, &uniqueListSSBO);
    glGenBuffers(1, &uniqueCountSSBO);
    glGenBuffers(1, &indirectArgsSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, uniqueCountSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_UNIQUE_COUNT_BINDING, uniqueCountSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, indirectArgsSSBO);   // uvec4[2]
    glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * 4 * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_INDIRECT_ARGS_BINDING, indirectArgsSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    shader::checkGLError("dedup buffers", success, *config);

    rrm.displaySize = config->framebufferSize();

    auto textureQuad = genQuad(glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 1.0f));

    glGenVertexArrays(1, &finalPass.VAO);
    glGenBuffers(1, &finalPass.VBO);
    glBindVertexArray(finalPass.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, finalPass.VBO);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), textureQuad.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    shader::checkGLError("Quad VAO/VBO", success, *config);

    glGenTextures(1, &primaryPass.texture);   // gbuffer (RGBA32UI)
    glGenTextures(1, &resolveTex);            // resolve output (RGBA32F)
    glGenTextures(1, &virtualGBufferTex);     // virtual gbuffer (RGBA32UI, downscaled)
    glGenTextures(1, &virtualDiffuseTex);     // virtual diffuse  (RGBA32F)
    glGenTextures(1, &virtualSpecularTex);    // virtual specular (RGBA32F)
    glGenTextures(1, &virtualNormalTex);      // virtual normal+roughness (RGBA32F: xyz=N, w=roughness)
    glGenTextures(1, &virtualSpecularTex2);   // specular à-trous ping-pong scratch

    glGenFramebuffers (1, &finalPass.framebuffer);
    glGenRenderbuffers(1, &finalPass.rbo);
    glGenTextures     (1, &finalPass.texture);

    shader::checkGLError("Texture/FBO allocation", success, *config);

    framebufferEvent();

    if (config->debuggingEnabled)
        config->logMessage("[%f] renderer init complete\n", glfwGetTime());
    shader::checkGLError("Renderer init end", success, *config);
}

bool Renderer::run(core::FrameConfig *frameConfig){
    profiler.beginFrame(glfwGetTime() * 1000.0);
    GLuint frameStamp = (GLuint)frameIdx + 1u;   // monotonic per-frame token (never 0 → 0 = empty slot)

    stats.scene_capacity = volume->capacity * sizeof(Octree::Node);
    stats.scene_mem      = volume->size     * sizeof(Octree::Node);
    stats.lBuffer_mem    = (uint32_t)LBUFFER_BYTES;
    stats.voxels_num     = volume->numVoxels;
    stats.cam_position   = camera->position;
    stats.cam_direction  = camera->direction;

    handleShaderRecompilation(frameConfig);

    glm::ivec2 prevDisplay = rrm.displaySize;
    rrm.displaySize = config->framebufferSize();
    if (rrm.displaySize != prevDisplay) {
        framebufferEvent();
        if (config->debuggingEnabled)
            config->logMessage("[%f] framebuffer resized\n", glfwGetTime());
        shader::checkGLError("Framebuffer resize", success, *config);
    }

    // Virtual framebuffer (re)size on first frame or when the scale slider changes.
    if (frameConfig->virtualScale != curVirtualScale) {
        curVirtualScale = frameConfig->virtualScale;
        allocVirtualTextures();
        shader::checkGLError("virtual texture resize", success, *config);
    }

    // Reset per-frame dedup state: clear the claim bitfield and uniqueVoxelCount.
    volume->clearClaimBitfield();
    {
        GLuint zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, uniqueCountSSBO);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // ---------------- primary.comp : DDA → gbuffer + dedup ----------------
    glBindImageTexture(0, primaryPass.texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    glUseProgram(primaryPass.program);
    {
        uint8_t unit = 1;
        volume->setProgram(primaryPass.program);
        volume->BindUniforms(unit);             // octree + claim SSBOs + octreeDepth
        glUniform2i(glGetUniformLocation(primaryPass.program, "screenResolution"),
                    rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1ui(glGetUniformLocation(primaryPass.program, "primary_raystop"),
                     (GLuint)frameConfig->primary_raystop);
    }
    profiler.start("primary");
    shader::dispatch(core::DispatchArgs::makeDirect(primaryPass.globalSize, primaryPass.groupSize));
    profiler.end("primary");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("primaryPass", success, *config);

    // buildArgs: indirectArgs[0] = ceil(uniqueCount / 64). One call covers claim, normal,
    // and avg — all dispatch per visible voxel over uniqueVoxelList (local 64). uniqueCount
    // is fixed after primary, and none of those passes modify indirectArgs.
    auto runBuildArgs = [&]() {
        glUseProgram(buildArgsPass.program);
        glUniform1ui(glGetUniformLocation(buildArgsPass.program, "localSize"), 64u);
        shader::dispatch(core::DispatchArgs::makeDirect(glm::ivec2(1, 1), glm::ivec2(1, 1)));
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    };
    runBuildArgs();

    // ---------------- lbuffer_claim : lock-free open-addressing (single dispatch) ----------------
    profiler.start("claim");
    glUseProgram(claimPass.program);
    glUniform1ui(glGetUniformLocation(claimPass.program, "frameStamp"), frameStamp);
    glUniform1ui(glGetUniformLocation(claimPass.program, "staleFrames"),
                 (GLuint)frameConfig->staleFrames);
    shader::dispatch(core::DispatchArgs::makeIndirect(indirectArgsSSBO, 0));
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    profiler.end("claim");
    shader::checkGLError("claimPass", success, *config);

    // ---------------- edit_mark.comp : re-flag edited regions (≤1 region/frame) ----------------
    volume->editExpandRadius = frameConfig->normalPrecision;
    profiler.start("editMark");
    if (volume->editLoggingEnabled && !volume->editRegions.empty()) {
        Octree::EditRegion r = volume->editRegions.front();
        volume->editRegions.pop_front();
        glm::uvec3 dim = r.max - r.min + glm::uvec3(1u);
        uint64_t   vol = (uint64_t)dim.x * dim.y * dim.z;
        const uint64_t CAP = 64ull * 64ull * 64ull;
        if (vol > CAP) {
            // Too big for one frame → split the longest axis, defer both halves.
            int ax = (dim.x >= dim.y && dim.x >= dim.z) ? 0 : (dim.y >= dim.z ? 1 : 2);
            uint32_t mid = (r.min[ax] + r.max[ax]) / 2u;
            Octree::EditRegion a = r, b = r;
            a.max[ax] = mid; b.min[ax] = mid + 1u;
            volume->editRegions.push_front(b);
            volume->editRegions.push_front(a);
        } else {
            glUseProgram(editMarkPass.program);
            glUniform1ui(glGetUniformLocation(editMarkPass.program, "octreeDepth"), (GLuint)volume->depth);
            glUniform3ui(glGetUniformLocation(editMarkPass.program, "regionMin"), r.min.x, r.min.y, r.min.z);
            glUniform3ui(glGetUniformLocation(editMarkPass.program, "regionMax"), r.max.x, r.max.y, r.max.z);
            glDispatchCompute((dim.x + 3u) / 4u, (dim.y + 3u) / 4u, (dim.z + 3u) / 4u);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            shader::checkGLError("editMarkPass", success, *config);
        }
    }
    profiler.end("editMark");

    // ---------------- normal.comp : per-voxel normal generation ----------------
    // (indirectArgs[0] from the buildArgs call above; claim/edit_mark don't change it)
    glUseProgram(normalPass.program);
    glUniform1ui(glGetUniformLocation(normalPass.program, "octreeDepth"), (GLuint)volume->depth);
    glUniform1ui(glGetUniformLocation(normalPass.program, "normalPrecision"),
                 (GLuint)frameConfig->normalPrecision);
    profiler.start("normal");
    shader::dispatch(core::DispatchArgs::makeIndirect(indirectArgsSSBO, 0));
    profiler.end("normal");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("normalPass", success, *config);

    // ---------------- downscale.comp : jittered gbuffer → virtual gbuffer ----------------
    glBindImageTexture(0, primaryPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, virtualGBufferTex,   0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    glUseProgram(downscalePass.program);
    {
        glUniform2i (glGetUniformLocation(downscalePass.program, "screenResolution"),
                     rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform2i (glGetUniformLocation(downscalePass.program, "virtualSize"),
                     virtualSize.x, virtualSize.y);
        glUniform1ui(glGetUniformLocation(downscalePass.program, "virtualScale"), (GLuint)curVirtualScale);
        glUniform1ui(glGetUniformLocation(downscalePass.program, "frameIndex"), (GLuint)frameIdx);
    }
    profiler.start("downscale");
    shader::dispatch(core::DispatchArgs::makeDirect(virtualSize, glm::ivec2(8, 8)));
    profiler.end("downscale");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    shader::checkGLError("downscalePass", success, *config);

    // ---------------- shade.comp : per-virtual-pixel path tracer ----------------
    glBindImageTexture(0, virtualGBufferTex,  0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, virtualDiffuseTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(2, virtualSpecularTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(3, virtualNormalTex,   0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glUseProgram(shadePass.program);
    {
        uint8_t unit = 1;
        volume->setProgram(shadePass.program);
        volume->BindUniforms(unit);            // octree SSBO (binding 0) + octreeDepth
        skybox->BindUniforms(shadePass.program, unit);
        glUniform1ui(glGetUniformLocation(shadePass.program, "primary_raystop"),
                     (GLuint)frameConfig->primary_raystop);
        glUniform1ui(glGetUniformLocation(shadePass.program, "frameIndex"), (GLuint)frameIdx);
        glUniform2i (glGetUniformLocation(shadePass.program, "virtualSize"),
                     virtualSize.x, virtualSize.y);
    }
    profiler.start("shade");
    shader::dispatch(core::DispatchArgs::makeDirect(virtualSize, glm::ivec2(8, 8)));
    profiler.end("shade");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("shadePass", success, *config);

    // ---------------- atrous.comp : adaptive specular-only edge-stopping filter (ping-pong) --------
    // Filters the virtual SPECULAR before accum only (diffuse is denoised by NEE/RIS + the long EMA
    // and is the sole fed-back channel, so it stays unfiltered). The per-pixel iteration budget
    // scales with roughness (virtualNormal.w): mirrors get 1 iter, glossy up to atrousIters.
    GLuint curSpec = virtualSpecularTex;
    if (frameConfig->atrousIters > 0) {
        GLuint scrSpec = virtualSpecularTex2;
        glUseProgram(atrousPass.program);
        glBindImageTexture(0, virtualGBufferTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
        glBindImageTexture(1, virtualNormalTex,  0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
        glUniform2i (glGetUniformLocation(atrousPass.program, "virtualSize"), virtualSize.x, virtualSize.y);
        glUniform1ui(glGetUniformLocation(atrousPass.program, "octreeDepth"), (GLuint)volume->depth);
        glUniform1f (glGetUniformLocation(atrousPass.program, "sigmaN"), frameConfig->atrousSigmaN);
        glUniform1f (glGetUniformLocation(atrousPass.program, "sigmaP"), frameConfig->atrousSigmaP);
        glUniform1i (glGetUniformLocation(atrousPass.program, "maxIters"), frameConfig->atrousIters);
        profiler.start("atrous");
        for (int it = 0; it < frameConfig->atrousIters; it++) {
            glUniform1i(glGetUniformLocation(atrousPass.program, "iter"), it);
            glBindImageTexture(2, curSpec, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);
            glBindImageTexture(3, scrSpec, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
            shader::dispatch(core::DispatchArgs::makeDirect(virtualSize, glm::ivec2(8, 8)));
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            GLuint ts = curSpec; curSpec = scrSpec; scrSpec = ts;   // ping-pong
        }
        profiler.end("atrous");
        shader::checkGLError("atrousPass", success, *config);
    }

    // ---------------- accum.comp : scatter virtual radiance → per-voxel accumulators ----------------
    glBindImageTexture(0, virtualGBufferTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
    glBindImageTexture(1, virtualDiffuseTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);   // diffuse: unfiltered
    glBindImageTexture(2, curSpec,           0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);   // specular: à-trous result
    glUseProgram(accumPass.program);
    glUniform2i(glGetUniformLocation(accumPass.program, "virtualSize"), virtualSize.x, virtualSize.y);
    profiler.start("accum");
    shader::dispatch(core::DispatchArgs::makeDirect(virtualSize, glm::ivec2(8, 8)));
    profiler.end("accum");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("accumPass", success, *config);

    // ---------------- avg.comp : per-voxel temporal EMA blend (indirect over uniqueList) ----------------
    // Reuses indirectArgs[0] built for normal.comp (same per-visible-voxel count, local 64).
    glUseProgram(avgPass.program);
    glUniform1ui(glGetUniformLocation(avgPass.program, "nDiffMax"), (GLuint)frameConfig->emaDiffuse);
    glUniform1ui(glGetUniformLocation(avgPass.program, "nSpecMax"), (GLuint)frameConfig->emaSpecular);
    profiler.start("avg");
    shader::dispatch(core::DispatchArgs::makeIndirect(indirectArgsSSBO, 0));
    profiler.end("avg");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("avgPass", success, *config);

    // ---------------- resolve.comp : gbuffer → resolveTex ----------------
    glBindImageTexture(0, primaryPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, resolveTex,          0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(2, virtualGBufferTex,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(3, virtualDiffuseTex,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);   // SHADE debug: unfiltered diffuse
    glBindImageTexture(4, curSpec,             0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);   // SHADE debug: à-trous specular
    glUseProgram(resolvePass.program);
    {
        uint8_t unit = 1;
        skybox->BindUniforms(resolvePass.program, unit);
        glUniform1ui(glGetUniformLocation(resolvePass.program, "octreeDepth"),
                     (GLuint)volume->depth);
        glUniform2i(glGetUniformLocation(resolvePass.program, "screenResolution"),
                    rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1ui(glGetUniformLocation(resolvePass.program, "renderMode"),
                     (GLuint)frameConfig->renderType);
        glUniform1ui(glGetUniformLocation(resolvePass.program, "frameStamp"),
                     frameStamp);
        glUniform1ui(glGetUniformLocation(resolvePass.program, "virtualScale"),
                     (GLuint)curVirtualScale);
        glUniform1ui(glGetUniformLocation(resolvePass.program, "primaryRaystop"),
                     (GLuint)frameConfig->primary_raystop);
    }
    profiler.start("resolve");
    shader::dispatch(core::DispatchArgs::makeDirect(primaryPass.globalSize, primaryPass.groupSize));
    profiler.end("resolve");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    shader::checkGLError("resolvePass", success, *config);


    // ---------------- final.frag --------------------------------
    glViewport(rrm.framebufferPos.x, rrm.framebufferPos.y,
               rrm.framebufferSize.x, rrm.framebufferSize.y);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (frameConfig->renderToTexture)
        glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(finalPass.program);
    {
        glUniform2i(glGetUniformLocation(finalPass.program, "screenResolution"),
                    rrm.framebufferSize.x, rrm.framebufferSize.y);
        glBindVertexArray(finalPass.VAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, resolveTex);
        glUniform1i(glGetUniformLocation(finalPass.program, "screenTexture"), 0);
    }

    profiler.start("final");
    glDrawArrays(GL_TRIANGLES, 0, 6);
    profiler.end("final");
    glBindTexture(GL_TEXTURE_2D, 0);
    shader::checkGLError("final pass", success, *config);

    profiler.endFrame();
    frameIdx++;

    if (config->debuggingEnabled)
        config->logMessage("[%f] frame end\n", glfwGetTime());
    return success;
}

Renderer::~Renderer(){
    camera->freeVRAM();
    volume->freeVRAM();
    materialPool->freeVRAM();

    glDeleteVertexArrays(1, &finalPass.VAO);
    glDeleteBuffers     (1, &finalPass.VBO);

    glDeleteBuffers(1, &lBufferSSBO);
    glDeleteBuffers(1, &uniqueListSSBO);
    glDeleteBuffers(1, &uniqueCountSSBO);
    glDeleteBuffers(1, &indirectArgsSSBO);
    glDeleteTextures(1, &primaryPass.texture);
    glDeleteTextures(1, &resolveTex);
    glDeleteTextures(1, &virtualGBufferTex);
    glDeleteTextures(1, &virtualDiffuseTex);
    glDeleteTextures(1, &virtualSpecularTex);
    glDeleteTextures(1, &virtualNormalTex);
    glDeleteTextures(1, &virtualSpecularTex2);
    glDeleteTextures(1, &finalPass.texture);

    glDeleteFramebuffers (1, &finalPass.framebuffer);
    glDeleteRenderbuffers(1, &finalPass.rbo);

    glDeleteProgram(primaryPass.program);
    glDeleteProgram(resolvePass.program);
    glDeleteProgram(buildArgsPass.program);
    glDeleteProgram(claimPass.program);
    glDeleteProgram(normalPass.program);
    glDeleteProgram(editMarkPass.program);
    glDeleteProgram(downscalePass.program);
    glDeleteProgram(shadePass.program);
    glDeleteProgram(accumPass.program);
    glDeleteProgram(avgPass.program);
    glDeleteProgram(atrousPass.program);
    glDeleteProgram(finalPass.program);
}

std::array<float, 24> Renderer::genQuad(glm::vec2 size, glm::vec2 tex) {
    return {{
        -size.x,  size.y,  0.0f,  tex.y,
        -size.x, -size.y,  0.0f,  0.0f,
         size.x, -size.y,  tex.x, 0.0f,
        -size.x,  size.y,  0.0f,  tex.y,
         size.x, -size.y,  tex.x, 0.0f,
         size.x,  size.y,  tex.x, tex.y
    }};
}

void Renderer::allocVoxelLists() {
    GLsizeiptr n = (GLsizeiptr)rrm.framebufferSize.x * (GLsizeiptr)rrm.framebufferSize.y;
    if (n <= 0) return;
    GLsizeiptr bytes = n * 4 * (GLsizeiptr)sizeof(GLuint);   // uvec4 per entry
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, uniqueListSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_UNIQUE_LIST_BINDING, uniqueListSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void Renderer::allocVirtualTextures() {
    if (curVirtualScale <= 0 || rrm.framebufferSize.x <= 0) return;
    virtualSize.x = (rrm.framebufferSize.x + curVirtualScale - 1) / curVirtualScale;
    virtualSize.y = (rrm.framebufferSize.y + curVirtualScale - 1) / curVirtualScale;

    glBindTexture(GL_TEXTURE_2D, virtualGBufferTex);   // downscaled gbuffer (RGBA32UI)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, virtualSize.x, virtualSize.y,
                 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint radTex[4] = { virtualDiffuseTex, virtualSpecularTex, virtualNormalTex,
                         virtualSpecularTex2 };   // RGBA32F virtual-res images
    for (int i = 0; i < 4; i++) {
        glBindTexture(GL_TEXTURE_2D, radTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, virtualSize.x, virtualSize.y,
                     0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
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

    primaryPass.globalSize = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    primaryPass.groupSize  = glm::ivec2(8, 8);

    glBindTexture(GL_TEXTURE_2D, primaryPass.texture);      // gbuffer (RGBA32UI)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindTexture(GL_TEXTURE_2D, resolveTex);               // resolve output (RGBA32F)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glBindTexture(GL_TEXTURE_2D, finalPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y,
                 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           finalPass.texture, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, finalPass.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          rrm.framebufferSize.x, rrm.framebufferSize.y);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, finalPass.rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        config->logMessage("RENDERER::ERROR::FRAMEBUFFER not complete!\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    allocVoxelLists();        // size the dedup lists to the framebuffer pixel count
    allocVirtualTextures();   // size the virtual gbuffer to ceil(framebuffer / virtualScale)
}

void Renderer::handleShaderRecompilation(core::FrameConfig *frameConfig) {
    if (!frameConfig->shaderRecompilation) return;

    shader::relinkCompute(primaryPass,   "./shd/primary.comp",       *config);
    shader::relinkCompute(resolvePass,   "./shd/resolve.comp",       *config);
    shader::relinkCompute(buildArgsPass, "./shd/buildArgs.comp",     *config);
    shader::relinkCompute(claimPass,     "./shd/lbuffer_claim.comp", *config);
    shader::relinkCompute(normalPass,    "./shd/normal.comp",        *config);
    shader::relinkCompute(editMarkPass,  "./shd/edit_mark.comp",     *config);
    shader::relinkCompute(downscalePass, "./shd/downscale.comp",     *config);
    shader::relinkCompute(shadePass,     "./shd/shade.comp",         *config);
    shader::relinkCompute(accumPass,     "./shd/accum.comp",         *config);
    shader::relinkCompute(avgPass,       "./shd/avg.comp",           *config);
    shader::relinkCompute(atrousPass,    "./shd/atrous.comp",        *config);
    shader::relinkRaster (finalPass,     "./shd/final.vert", "./shd/final.frag", *config);

    camera      ->setProgram(primaryPass.program);
    camera      ->setProgram(resolvePass.program);
    camera      ->setProgram(shadePass.program);
    materialPool->setProgram(resolvePass.program);
    materialPool->setProgram(shadePass.program);

    frameConfig->shaderRecompilation = false;
    if (config->debuggingEnabled)
        config->logMessage("[%f] shaders recompiled\n", glfwGetTime());
    shader::checkGLError("shader recompilation", success, *config);
}
