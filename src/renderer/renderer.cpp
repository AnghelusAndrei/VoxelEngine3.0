#include "renderer.hpp"

static uint32_t gcd_u(uint32_t a, uint32_t b){ while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

// Pick the deterministic-jitter stride K for a given downscale `scale`
static uint32_t pickJitterStride(uint32_t S, uint32_t scale) {
    uint32_t k0 = (uint32_t)(((uint64_t)S * 618034ull) / 1000000ull);
    if (k0 == 0u) k0 = 1u;
    uint32_t fallback = 0u;
    for (uint32_t i = 0u; i < S; ++i) {
        uint32_t c = ((k0 + i - 1u) % S) + 1u;          // sweep [1..S], starting near golden
        if (gcd_u(c, scale) != 1u) continue;
        if (fallback == 0u) fallback = c;
        uint32_t m = c % scale;
        if (scale > 3u && (m == 0u || m == 1u || m == scale - 1u)) continue;
        return c;                                       // coprime AND well-spread on both axes
    }
    return fallback ? fallback : 1u;
}

static void allocTex2D(GLuint tex, GLint internalFmt, GLenum fmt, GLenum type,
                       glm::ivec2 size, GLint filter = GL_NEAREST) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, size.x, size.y, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

shader::Defines Renderer::shaderDefines() const {
    return shader::Defines()
        .include("constants.glsl")
        .define("MAXDEPTH", (long long)volume->depth + 1);
}

Renderer::Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_,
                   MaterialPool *materialPool_, Skybox *skybox_) :
                   config(config_), volume(volume_), camera(camera_),
                   materialPool(materialPool_), skybox(skybox_)
{
    config->logMessage("GL version: %s\n", glGetString(GL_VERSION));

    if (config->debuggingEnabled)
        config->logMessage("[%f] initializing the renderer\n", glfwGetTime());
    shader::checkGLError("Renderer initialization", success, *config);


    success &= passes.linkAll(*config);

    if (config->debuggingEnabled)
        config->logMessage("[%f] compiled shaders\n", glfwGetTime());
    shader::checkGLError("Shader linking", success, *config);

    camera      ->GenUBO();
    volume      ->GenUBO();
    materialPool->GenUBO();

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, camera->gl_ID);

    // lBuffer radiance cache (SSBO). Zero-initialised, bound persistently. Flat,
    // open-addressed, 16-DWORD slots; claimed lock-free via CAS on the timestamp word
    // (see lbuffer.glsl / architecture/CLAIM.md). Zero = pristine-empty.
    glGenBuffers(1, &lBufferSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, lBufferSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, LBUFFER_BYTES, NULL, GL_DYNAMIC_DRAW);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_LBUFFER_BINDING, lBufferSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (config->debuggingEnabled)
        config->logMessage("[%f] initialized lBuffer SSBO\n", glfwGetTime());
    shader::checkGLError("lBuffer SSBO", success, *config);

    // Dedup pipeline buffers. uniqueList is sized in allocVoxelLists() (via
    // framebufferEvent); the small count / args are fixed-size and allocated here.
    glGenBuffers(1, &uniqueListSSBO);
    glGenBuffers(1, &accumDiffuseSSBO);
    glGenBuffers(1, &accumSpecularSSBO);
    glGenBuffers(1, &uniqueCountSSBO);
    glGenBuffers(1, &indirectArgsSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, uniqueCountSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_UNIQUE_COUNT_BINDING, uniqueCountSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, indirectArgsSSBO);   // uvec4[2]
    glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * 4 * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_INDIRECT_ARGS_BINDING, indirectArgsSSBO);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (config->debuggingEnabled)
        config->logMessage("[%f] initialized dedup buffers SSBO\n", glfwGetTime());
    shader::checkGLError("dedup buffers", success, *config);

    displaySize = config->framebufferSize();

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

    glGenTextures(1, &gbufferTex);   // gbuffer (RGBA32UI)
    glGenTextures(1, &resolveTex);            // resolve output (RGBA32F)
    glGenTextures(1, &virtualGBufferTex);     // virtual gbuffer (RGBA32UI, downscaled)
    glGenTextures(1, &virtualDiffuseTex);     // virtual diffuse  (RGBA32F)
    glGenTextures(1, &virtualSpecularTex);    // virtual specular (RGBA32F)
    glGenTextures(1, &holeFillTex);           // gathered cache-hole fill (RGBA32UI)

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

    // ---- cache freeze (CLAIM_AGE / LRU_OCCUPANCY) --------------------------------
    const bool freezeCache = (frameConfig->renderType == core::CLAIM_AGE ||
                              frameConfig->renderType == core::LRU_OCCUPANCY);
    if (freezeCache) {
        if (!cacheFrozen) { frozenStamp = frameStamp; cacheFrozen = true; }
        frameStamp = frozenStamp;
    } else {
        cacheFrozen = false;
    }

    stats.scene_capacity = volume->capacity * sizeof(Octree::Node);
    stats.scene_mem      = volume->size     * sizeof(Octree::Node);
    stats.lBuffer_mem    = (uint32_t)LBUFFER_BYTES;
    stats.voxels_num     = volume->numVoxels;
    stats.cam_position   = camera->position;
    stats.cam_direction  = camera->direction;

    
    if (frameConfig->shaderRecompilation){
        passes.reloadAll(*config);
        frameConfig->shaderRecompilation = false;
        if (config->debuggingEnabled)
            config->logMessage("[%f] shaders recompiled\n", glfwGetTime());
        shader::checkGLError("shader recompilation", success, *config);
    }

    glm::ivec2 prevDisplay = displaySize;
    displaySize = config->framebufferSize();
    if (displaySize != prevDisplay || frameConfig->virtualScale != curVirtualScale) {
        curVirtualScale = frameConfig->virtualScale;
        framebufferEvent();   // resizes the gbuffer/resolve/final targets AND the virtual textures
        if (config->debuggingEnabled)
            config->logMessage("[%f] framebuffer + virtual resized\n", glfwGetTime());
        shader::checkGLError("Framebuffer + virtual resize", success, *config);
        const GLuint scale = (GLuint)(curVirtualScale > 0 ? curVirtualScale : 1);
        jitterPeriod = scale * scale;
        jitterStride = pickJitterStride(jitterPeriod, scale);
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

    // ---------------- primary.comp : DDA → gbuffer ----------------
    glBindImageTexture(0, gbufferTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    {
        volume->BindUniforms(primaryPass.program());   // octree + claim SSBOs + octreeDepth
        primaryPass.set("screenResolution", glm::ivec2(framebufferSize.x, framebufferSize.y));
        primaryPass.set("primary_raystop", (GLuint)frameConfig->primary_raystop);
    }
    profiler.start("primary");
    primaryPass.dispatch(fullRes());
    profiler.end("primary");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("primaryPass", success, *config);

    // ---------------- downscale.comp : jittered gbuffer → virtual gbuffer ----------------
    glBindImageTexture(0, gbufferTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, virtualGBufferTex,   0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    {
        downscalePass.set("screenResolution", glm::ivec2(framebufferSize.x, framebufferSize.y));
        downscalePass.set("virtualSize", glm::ivec2(virtualSize.x, virtualSize.y));
        downscalePass.set("virtualScale", (GLuint)curVirtualScale);
        downscalePass.set("frameIndex", (GLuint)frameIdx);
        downscalePass.set("jitterStride", (GLuint)jitterStride);
    }
    profiler.start("downscale");
    downscalePass.dispatch(virtualRes());
    profiler.end("downscale");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("downscalePass", success, *config);

    // buildArgs: indirectArgs[0] = ceil(uniqueCount / 64). One call covers claim, normal,
    // and avg — all dispatch per visible voxel over uniqueVoxelList (local 64). uniqueCount
    // is fixed after primary, and none of those passes modify indirectArgs.
    auto runBuildArgs = [&]() {
        buildArgsPass.set("localSize", (GLuint)64u);
        buildArgsPass.dispatch(single());
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    };
    runBuildArgs();

    // Everything from here to avg writes the lBuffer, so the freeze brackets the lot.
    if (!freezeCache) {
    // ---------------- lbuffer_claim : lock-free open-addressing (single dispatch) ----------------
    profiler.start("claim");
    claimPass.set("frameStamp", (GLuint)frameStamp);
    claimPass.set("staleViewDep", (GLuint)frameConfig->staleViewDep);
    claimPass.set("staleViewIndep", (GLuint)frameConfig->staleViewIndep);
    claimPass.dispatch(perVoxel());
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
            editMarkPass.set("octreeDepth", (GLuint)volume->depth);
            editMarkPass.set("regionMin", glm::uvec3(r.min.x, r.min.y, r.min.z));
            editMarkPass.set("regionMax", glm::uvec3(r.max.x, r.max.y, r.max.z));
            editMarkPass.dispatch(shader::Dispatch::direct(glm::ivec3(dim), glm::ivec3(4)));
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            shader::checkGLError("editMarkPass", success, *config);
        }
    }
    profiler.end("editMark");

    // ---------------- normal.comp : per-voxel normal generation ----------------
    // (indirectArgs[0] from the buildArgs call above; claim/edit_mark don't change it)
    normalPass.set("octreeDepth", (GLuint)volume->depth);
    normalPass.set("normalPrecision", (GLuint)frameConfig->normalPrecision);
    profiler.start("normal");
    normalPass.dispatch(perVoxel());
    profiler.end("normal");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("normalPass", success, *config);

    // ---------------- shade.comp : per-virtual-pixel path tracer ----------------
    // READ_WRITE: shade stamps each texel's uniqueVoxelList index into the gbuffer's spare
    // bits for accum (gbuffer.glsl). Every invocation touches only its own texel.
    glBindImageTexture(0, virtualGBufferTex,  0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32UI);
    glBindImageTexture(1, virtualDiffuseTex,  0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(2, virtualSpecularTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    {
        uint8_t unit = 1;
        volume->BindUniforms(shadePass.program());     // octree SSBO (binding 0) + octreeDepth
        skybox->BindUniforms(shadePass.program(), unit);
        shadePass.set("primary_raystop", (GLuint)frameConfig->primary_raystop);
        shadePass.set("frameIndex", (GLuint)frameIdx);
        shadePass.set("virtualSize", glm::ivec2(virtualSize.x, virtualSize.y));
    }
    profiler.start("shade");
    shadePass.dispatch(virtualRes());
    profiler.end("shade");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("shadePass", success, *config);


    // ---------------- accum.comp : scatter virtual radiance → per-voxel accumulators ----------------
    glBindImageTexture(0, virtualGBufferTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
    glBindImageTexture(1, virtualDiffuseTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);   // diffuse: unfiltered
    glBindImageTexture(2, virtualSpecularTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);   // specular: unfiltered
    accumPass.set("virtualSize", glm::ivec2(virtualSize.x, virtualSize.y));
    profiler.start("accum");
    accumPass.dispatch(virtualRes());
    profiler.end("accum");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("accumPass", success, *config);

    // ---------------- avg.comp : per-voxel temporal EMA blend (indirect over uniqueList) ----------------
    avgPass.set("nDiffMax", (GLuint)frameConfig->emaDiffuse);
    avgPass.set("nSpecMax", (GLuint)frameConfig->emaSpecular);
    profiler.start("avg");
    avgPass.dispatch(perVoxel());
    profiler.end("avg");
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    shader::checkGLError("avgPass", success, *config);

    }   // end !freezeCache

    // ---------------- holefill.comp : per-virtual-texel cache-hole gather ----------------
    glBindImageTexture(0, virtualGBufferTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, holeFillTex,       0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
    {
        holefillPass.set("virtualSize", glm::ivec2(virtualSize.x, virtualSize.y));
        holefillPass.set("frameStamp", (GLuint)frameStamp);
        holefillPass.set("staleViewDep", (GLuint)frameConfig->staleViewDep);
        holefillPass.set("staleViewIndep", (GLuint)frameConfig->staleViewIndep);
    }
    profiler.start("holefill");
    holefillPass.dispatch(virtualRes());
    profiler.end("holefill");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    shader::checkGLError("holefillPass", success, *config);

    // ---------------- resolve.comp : gbuffer → resolveTex ----------------
    glBindImageTexture(0, gbufferTex, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(1, resolveTex,          0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glBindImageTexture(2, virtualGBufferTex,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);
    glBindImageTexture(3, virtualDiffuseTex,   0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);   // SHADE debug: unfiltered diffuse
    glBindImageTexture(4, virtualSpecularTex,  0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);   // SHADE debug: unfiltered specular
    glBindImageTexture(5, holeFillTex,         0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32UI);  // gathered cache-hole fill
    {
        uint8_t unit = 1;
        skybox->BindUniforms(resolvePass.program(), unit);
        resolvePass.set("octreeDepth", (GLuint)volume->depth);
        resolvePass.set("screenResolution", glm::ivec2(framebufferSize.x, framebufferSize.y));
        resolvePass.set("renderMode", (GLuint)frameConfig->renderType);
        resolvePass.set("frameStamp", (GLuint)frameStamp);
        resolvePass.set("virtualScale", (GLuint)curVirtualScale);
        resolvePass.set("virtualSize", glm::ivec2(virtualSize.x, virtualSize.y));
        resolvePass.set("staleViewDep", (GLuint)frameConfig->staleViewDep);
        resolvePass.set("staleViewIndep", (GLuint)frameConfig->staleViewIndep);
    }
    profiler.start("resolve");
    resolvePass.dispatch(fullRes());
    profiler.end("resolve");
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    shader::checkGLError("resolvePass", success, *config);


    // ---------------- final.frag --------------------------------
    glViewport(framebufferPos.x, framebufferPos.y,
               framebufferSize.x, framebufferSize.y);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (frameConfig->renderToTexture)
        glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    finalPass.set("screenResolution", glm::ivec2(framebufferSize.x, framebufferSize.y));
    finalPass.set("screenTexture", (GLint)0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolveTex);

    profiler.start("final");
    finalPass.draw();
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
    glDeleteBuffers(1, &accumDiffuseSSBO);
    glDeleteBuffers(1, &accumSpecularSSBO);
    glDeleteBuffers(1, &uniqueCountSSBO);
    glDeleteBuffers(1, &indirectArgsSSBO);
    glDeleteTextures(1, &gbufferTex);
    glDeleteTextures(1, &resolveTex);
    glDeleteTextures(1, &virtualGBufferTex);
    glDeleteTextures(1, &virtualDiffuseTex);
    glDeleteTextures(1, &virtualSpecularTex);
    glDeleteTextures(1, &holeFillTex);
    // Each pass tears down its own program, and FinalRasterPass its quad and target.
    passes.destroyAll();
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
    GLsizeiptr n = (GLsizeiptr)virtualSize.x * (GLsizeiptr)virtualSize.y;
    if (n <= 0) return;

    // The list index rides in whatever bits the material field leaves over in the virtual
    // gbuffer (gbuffer.glsl: VG_LISTIDX_NONE). Widening material narrows this, so the
    // ceiling moves with it - unreachable at any sane virtualScale, but silent if hit.
    if (n >= 0x3FFFFF)   // == VG_LISTIDX_NONE, the 22-bit sentinel
        config->logMessage("RENDERER::ERROR::virtual framebuffer too large for the 22-bit "
                           "list index (%lld texels)\n", (long long)n);

    const GLsizeiptr bytes = n * 4 * (GLsizeiptr)sizeof(GLuint);
    const GLuint     zero  = 0u;

    struct { GLuint id; GLuint binding; bool clear; } bufs[] = {
        { uniqueListSSBO,    core::SSBO_UNIQUE_LIST_BINDING,    false },  // fully rewritten each frame
        { accumDiffuseSSBO,  core::SSBO_ACCUM_DIFFUSE_BINDING,  true  },  // atomicAdd target: must start at 0
        { accumSpecularSSBO, core::SSBO_ACCUM_SPECULAR_BINDING, true  },
    };
    for (auto& b : bufs) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.id);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, NULL, GL_DYNAMIC_DRAW);
        if (b.clear)
            glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                              GL_UNSIGNED_INT, &zero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b.binding, b.id);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void Renderer::allocVirtualTextures() {
    if (curVirtualScale <= 0 || framebufferSize.x <= 0) return;
    virtualSize.x = (framebufferSize.x + curVirtualScale - 1) / curVirtualScale;
    virtualSize.y = (framebufferSize.y + curVirtualScale - 1) / curVirtualScale;

    allocTex2D(virtualGBufferTex, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, virtualSize);
    allocTex2D(virtualDiffuseTex, GL_RGBA32F,  GL_RGBA,         GL_FLOAT, virtualSize);
    allocTex2D(virtualSpecularTex, GL_RGBA32F, GL_RGBA,         GL_FLOAT, virtualSize);
    allocTex2D(holeFillTex, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, virtualSize);
}

void Renderer::framebufferEvent() {
    const float windowAspect = (float)displaySize.x / (float)displaySize.y;

    if (windowAspect > config->aspectRatio) {
        framebufferSize.y = displaySize.y;
        framebufferSize.x = static_cast<int>(displaySize.y * config->aspectRatio);
        framebufferPos.x  = (displaySize.x - framebufferSize.x) / 2;
        framebufferPos.y  = 0;
    } else {
        framebufferSize.x = displaySize.x;
        framebufferSize.y = static_cast<int>(displaySize.x / config->aspectRatio);
        framebufferPos.x  = 0;
        framebufferPos.y  = (displaySize.y - framebufferSize.y) / 2;
    }


    allocTex2D(gbufferTex, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, framebufferSize);
    allocTex2D(resolveTex, GL_RGBA32F,  GL_RGBA,         GL_FLOAT, framebufferSize, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    allocTex2D(finalPass.texture, GL_RGBA32F,  GL_RGBA,         GL_FLOAT, framebufferSize, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, finalPass.texture, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, finalPass.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, framebufferSize.x, framebufferSize.y);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, finalPass.rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        config->logMessage("RENDERER::ERROR::FRAMEBUFFER not complete!\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    config->logMessage("Window size: %dx%d\n", displaySize.x, displaySize.y);

    allocVirtualTextures();   // size the virtual gbuffer to ceil(framebuffer / virtualScale)
    allocVoxelLists();        // size the dedup list to the full-res pixel count (primary appends per full-res pixel)
}