#pragma once
#include "core.hpp"
#include "octree.hpp"
#include "camera.hpp"
#include "material.hpp"

class Renderer{
    public:
    Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_, MaterialPool *materialPool_);
    bool run(core::FrameConfig *frameConfig);
    ~Renderer();

    core::RendererConfig *config;
    core::DebugInfo debug;

    core::RasterPass rayPass;
    core::RasterPass finalPass;

    const char* glsl_version = "#version 430";
    private:
    bool success = true;
    core::runtimeRendererMem rrm;

    core::lightingBuffer lBuffer;
 
    core::ComputePass accumPass;
    core::ComputePass avgPass;

    core::RenderType currentRenderType = core::RenderType::DEFAULT;

    Octree *volume;
    Camera *camera;
    MaterialPool *materialPool;

    void framebufferEvent();
    void handleShaderRecompilation(core::FrameConfig *frameConfig);
    float* genQuad(glm::vec2 size, glm::vec2 tex);
    void linkCompute(core::ComputePass *pass, const char *shaderFile);
    void linkRaster(core::RasterPass *pass, const char *vertexFile, const char *fragmentFile);
    GLuint compileShader(const char* path, std::string type, GLuint gl_type);
    void relinkCompute(core::ComputePass *pass, const char *shaderFile);
    void relinkRaster(core::RasterPass *pass, const char *vertexFile, const char *fragmentFile);
    void checkProgramCompileErrors(unsigned int shader);
    void checkGLError(bool *succes);
};