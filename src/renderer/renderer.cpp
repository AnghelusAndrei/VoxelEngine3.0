#include "renderer.hpp"
#include <stdio.h>
#include <string.h>


Renderer::Renderer(core::RendererConfig *config_, Octree *volume_, Camera *camera_, MaterialPool *materialPool_) : config(config_), volume(volume_), camera(camera_), materialPool(materialPool_){

    lBuffer.stride = 10;//fixed size, determines the slot layout used in the pipeline
    lBuffer.slots = 10; //variable size, determines the number of voxel slots in the lighting buffer
    lBuffer.instruction = 1;

    if(config->debuggingEnabled)config->logMessage("[%f] initializing the renderer \n", glfwGetTime());
    checkGLError(&success);

    linkRaster(&rayPass, "./shd/ray.vert", "./shd/ray.frag");
    linkCompute(&accumPass, "./shd/accum.comp");
    linkCompute(&avgPass, "./shd/avg.comp");
    linkRaster(&finalPass, "./shd/final.vert", "./shd/final.frag");

    if(config->debuggingEnabled)config->logMessage("[%f] compiled shaders \n", glfwGetTime());
    checkGLError(&success);

    camera->GenUBO(rayPass.program);
    volume->GenUBO(rayPass.program);
    materialPool->GenUBO(rayPass.program);

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

    glGenVertexArrays(1, &rayPass.VAO);
    glGenBuffers(1, &rayPass.VBO);
    glBindVertexArray(rayPass.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, rayPass.VBO);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), textureQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    if(config->debuggingEnabled)config->logMessage("[%f] generated Quad Vertex Array Objects \n", glfwGetTime());
    checkGLError(&success);

    glGenTextures(1, &avgPass.texture);
    
    glGenFramebuffers(1, &rayPass.framebuffer);
    glGenTextures(1, &rayPass.texture);
    glGenRenderbuffers(1, &rayPass.rbo);

    glGenFramebuffers(1, &finalPass.framebuffer);
    glGenTextures(1, &finalPass.texture);
    glGenRenderbuffers(1, &finalPass.rbo);

    if(config->debuggingEnabled)config->logMessage("[%f] building lighting buffer \n", glfwGetTime());
    checkGLError(&success);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &lBuffer.size.x);
    lBuffer.size.y = lBuffer.stride * lBuffer.slots;

    glGenTextures(1, &lBuffer.texture);
    glBindTexture(GL_TEXTURE_2D, lBuffer.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, lBuffer.size.x, lBuffer.size.y, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if(config->debuggingEnabled)config->logMessage("[%f] built lighting buffer \n", glfwGetTime());
    checkGLError(&success);

    framebufferEvent();

    if(config->debuggingEnabled)config->logMessage("[%f] generated all framebuffer objects \n", glfwGetTime());
    checkGLError(&success);
}

bool Renderer::run(core::FrameConfig *frameConfig){
    
    debug.gpu_start_ms = glfwGetTime() * 1000.0;
    debug.start_ms = debug.end_ms;
    debug.end_ms = glfwGetTime() * 1000.0;

    handleShaderRecompilation(frameConfig);

    debug.gpu_shaderCompilation_ms = glfwGetTime() * 1000.0;

    glm::ivec2 displayComparaison = rrm.displaySize;
    rrm.displaySize = config->framebufferSize();

    if(rrm.displaySize != displayComparaison){
        framebufferEvent();
        if(config->debuggingEnabled)config->logMessage("[%f] framebuffer resized \n", glfwGetTime());
        checkGLError(&success);
    }

    debug.gpu_framebufferResize_ms = glfwGetTime() * 1000.0;
    
    //rayPass

    // Set the viewport
    glViewport(0, 0, rrm.framebufferSize.x, rrm.framebufferSize.y);

    glBindFramebuffer(GL_FRAMEBUFFER, rayPass.framebuffer);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    
    glUseProgram(rayPass.program);

    {
        rrm.texturesBound = 0;
        volume->BindUniforms(rrm.texturesBound);
        
        GLint resLoc = glGetUniformLocation(rayPass.program, "screenResolution");
        GLint timeLoc = glGetUniformLocation(rayPass.program, "time");
        GLint sppLoc = glGetUniformLocation(rayPass.program, "spp");
        GLint bouncesLoc = glGetUniformLocation(rayPass.program, "lightBounces");

        glUniform2i(resLoc, rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1i(timeLoc, (int)(glfwGetTime()*10000));
        glUniform1i(sppLoc, frameConfig->spp);
        glUniform1i(bouncesLoc, frameConfig->bounces);
    }

    glBindVertexArray(rayPass.VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    debug.gpu_pass1_ms = glfwGetTime() * 1000.0;
    if(config->debuggingEnabled)config->logMessage("[%f] pass 1 \n", glfwGetTime());
    checkGLError(&success);

    //accumPass

    #define ADDLEFT 1
    #define ADDRIGHT 2
    #define ADDCLEARLEFT 3
    #define ADDCLEARRIGHT 4

    glUseProgram(accumPass.program);
    glBindImageTexture(0, rayPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(1, lBuffer.texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    {
        GLint resLoc = glGetUniformLocation(accumPass.program, "screenResolution");
        GLint slotsLoc = glGetUniformLocation(accumPass.program, "slots");
        GLint strideLoc = glGetUniformLocation(accumPass.program, "stride");
        GLint timeLoc = glGetUniformLocation(accumPass.program, "time");
        GLint updateLoc = glGetUniformLocation(accumPass.program, "updateTime");

        glUniform2i(resLoc, rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1i(slotsLoc, lBuffer.slots);
        glUniform1i(strideLoc, lBuffer.stride);
        glUniform1ui(timeLoc, (GLuint)(glfwGetTime()*100));
        glUniform1ui(updateLoc, (GLuint)(2.0 * (debug.end_ms - debug.start_ms)));

        GLint instructionLoc = glGetUniformLocation(accumPass.program, "instruction");
        if(glfwGetTime() - lBuffer.accumulationTime > frameConfig->lBufferSwapSeconds && !(frameConfig->TAA)){
            lBuffer.accumulationTime = glfwGetTime();
            switch(lBuffer.instruction){
                case ADDRIGHT:
                    glUniform1i(instructionLoc, ADDCLEARLEFT);
                    lBuffer.instruction = ADDLEFT;
                    break;
                case ADDLEFT:
                    glUniform1i(instructionLoc, ADDCLEARRIGHT);
                    lBuffer.instruction = ADDRIGHT;
                    break;
            }
        }else{
            glUniform1i(instructionLoc, lBuffer.instruction);
        }
    }
    glDispatchCompute((GLuint)ceil((float)accumPass.globalSize.x / (float)accumPass.groupSize.x), (GLuint)ceil((float)accumPass.globalSize.y / (float)accumPass.groupSize.y), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    debug.gpu_pass2_ms = glfwGetTime() * 1000.0;
    if(config->debuggingEnabled)config->logMessage("[%f] pass 2 \n", glfwGetTime());
    checkGLError(&success);

    //avgPass

    glUseProgram(avgPass.program);
    glBindImageTexture(0, lBuffer.texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glBindImageTexture(1, rayPass.texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(2, avgPass.texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    {
        GLint resLoc = glGetUniformLocation(avgPass.program, "screenResolution");
        GLint slotsLoc = glGetUniformLocation(avgPass.program, "slots");
        GLint strideLoc = glGetUniformLocation(avgPass.program, "stride");

        glUniform2i(resLoc, rrm.framebufferSize.x, rrm.framebufferSize.y);
        glUniform1i(slotsLoc, lBuffer.slots);
        glUniform1i(strideLoc, lBuffer.stride);
    }
    glDispatchCompute((GLuint)ceil((float)avgPass.globalSize.x / (float)avgPass.groupSize.x), (GLuint)ceil((float)avgPass.globalSize.y / (float)avgPass.groupSize.y), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    debug.gpu_pass3_ms = glfwGetTime() * 1000.0;
    if(config->debuggingEnabled)config->logMessage("[%f] pass 3 \n", glfwGetTime());
    checkGLError(&success);

    //finalPass

    // Set the viewport
    glViewport(rrm.framebufferPos.x, rrm.framebufferPos.y, rrm.framebufferSize.x, rrm.framebufferSize.y);

    // now bind back to default framebuffer and draw a quad plane with the attached framebuffer color texture
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if(frameConfig->renderToTexture)
        glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glDisable(GL_DEPTH_TEST); // disable depth test so screen-space quad isn't discarded due to depth test.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(finalPass.program);

    {
        GLint resLoc = glGetUniformLocation(finalPass.program, "screenResolution");
        glUniform2i(resLoc, rrm.framebufferSize.x, rrm.framebufferSize.y);
    }

    glBindVertexArray(finalPass.VAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, avgPass.texture);    // use the color attachment texture as the texture of the quad plane
    glUniform1i(glGetUniformLocation(finalPass.program, "screenTexture"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Unbind the textures after drawing
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    debug.gpu_end_ms = glfwGetTime() * 1000.0;
    if(config->debuggingEnabled)config->logMessage("[%f] frame end \n", glfwGetTime());
    checkGLError(&success);

    return success; 
}

Renderer::~Renderer(){
    camera->freeVRAM();
    volume->freeVRAM();
    materialPool->freeVRAM();

    glDeleteVertexArrays(1, &finalPass.VAO);
    glDeleteBuffers(1, &finalPass.VBO);

    glDeleteVertexArrays(1, &rayPass.VAO);
    glDeleteBuffers(1, &rayPass.VBO);
    glDeleteTextures(1, &rayPass.texture);
    glDeleteTextures(1, &lBuffer.texture);
    glDeleteTextures(1, &avgPass.texture);
    glDeleteRenderbuffers(1, &rayPass.rbo);
    glDeleteFramebuffers(1, &rayPass.framebuffer);

    glDeleteProgram(rayPass.program);
    glDeleteProgram(accumPass.program);
    glDeleteProgram(accumPass.program);
    glDeleteProgram(finalPass.program);
}

float* Renderer::genQuad(glm::vec2 size, glm::vec2 tex){
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

void Renderer::framebufferEvent(){

    // Calculate the aspect ratio of the window
    float windowAspect = (float)rrm.displaySize.x / (float)rrm.displaySize.y;

    if (windowAspect > config->aspectRatio) {
        // Window is wider than desired aspect ratio
        rrm.framebufferSize.y = rrm.displaySize.y;
        rrm.framebufferSize.x = static_cast<int>(rrm.displaySize.y * config->aspectRatio);
        rrm.framebufferPos.x = (rrm.displaySize.x - rrm.framebufferSize.x) / 2;
        rrm.framebufferPos.y = 0;
    } else {
        // Window is taller than desired aspect ratio
        rrm.framebufferSize.x = rrm.displaySize.x;
        rrm.framebufferSize.y = static_cast<int>(rrm.displaySize.x / config->aspectRatio);
        rrm.framebufferPos.x = 0;
        rrm.framebufferPos.y = (rrm.displaySize.y - rrm.framebufferSize.y) / 2;
    }

    accumPass.globalSize = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    accumPass.groupSize = glm::ivec2(8, 8);
    avgPass.globalSize = glm::ivec2(rrm.framebufferSize.x, rrm.framebufferSize.y);
    avgPass.groupSize = glm::ivec2(8, 8);

    glBindFramebuffer(GL_FRAMEBUFFER, rayPass.framebuffer);
    glBindTexture(GL_TEXTURE_2D, rayPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rayPass.texture, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, rayPass.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rrm.framebufferSize.x, rrm.framebufferSize.y); // depth and stencil buffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rayPass.rbo); // attach it

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        config->logMessage("RENDERER::ERROR::FRAMEBUFFER:: Framebuffer is not complete! \n");

    glBindFramebuffer(GL_FRAMEBUFFER, finalPass.framebuffer);
    glBindTexture(GL_TEXTURE_2D, finalPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, finalPass.texture, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, finalPass.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rrm.framebufferSize.x, rrm.framebufferSize.y); // depth and stencil buffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, finalPass.rbo); // attach it

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        config->logMessage("RENDERER::ERROR::FRAMEBUFFER:: Framebuffer is not complete! \n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glBindTexture(GL_TEXTURE_2D, avgPass.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rrm.framebufferSize.x, rrm.framebufferSize.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::handleShaderRecompilation(core::FrameConfig *frameConfig){
    if(currentRenderType != frameConfig->renderType){
        switch (frameConfig->renderType)
        {
        case core::RenderType::DEFAULT :
            relinkRaster(&rayPass, "./shd/ray.vert", "./shd/ray.frag");
            volume->setProgram(rayPass.program);
            camera->setProgram(rayPass.program);
            materialPool->setProgram(rayPass.program);
            break;
        case core::RenderType::NORMAL :
            relinkRaster(&rayPass, "./shd/ray.vert", "./shd/normal_ray.frag");
            volume->setProgram(rayPass.program);
            camera->setProgram(rayPass.program);
            materialPool->setProgram(rayPass.program);
            break;
        case core::RenderType::STRUCTURE :
            relinkRaster(&rayPass, "./shd/ray.vert", "./shd/accel_struct_ray.frag");
            volume->setProgram(rayPass.program);
            camera->setProgram(rayPass.program);
            materialPool->setProgram(rayPass.program);
            break;     
        case core::RenderType::ALBEDO :
            relinkRaster(&rayPass, "./shd/ray.vert", "./shd/albedo_ray.frag");
            volume->setProgram(rayPass.program);
            camera->setProgram(rayPass.program);
            materialPool->setProgram(rayPass.program);
            break;    
        case core::RenderType::VOXELID :
            relinkRaster(&rayPass, "./shd/ray.vert", "./shd/voxelid_ray.frag");
            volume->setProgram(rayPass.program);
            camera->setProgram(rayPass.program);
            materialPool->setProgram(rayPass.program);
            break;    
        }
        currentRenderType = frameConfig->renderType;
        frameConfig->TAA = false;
        if(config->debuggingEnabled)config->logMessage("[%f] recompiled shaders \n", glfwGetTime());
        checkGLError(&success);
    }

    if(frameConfig->shaderRecompilation){
        relinkRaster(&rayPass, "./shd/ray.vert", "./shd/ray.frag");
        volume->setProgram(rayPass.program);
        camera->setProgram(rayPass.program);
        materialPool->setProgram(rayPass.program);
        relinkCompute(&accumPass, "./shd/accum.comp");
        relinkCompute(&avgPass, "./shd/avg.comp");
        //relinkRaster(&rayPass, "./shd/final.vert", "./shd/final.frag");
        frameConfig->shaderRecompilation = false;
        frameConfig->TAA = false;
        if(config->debuggingEnabled)config->logMessage("[%f] recompiled shaders \n", glfwGetTime());
        checkGLError(&success);
    }
}

void Renderer::linkCompute(core::ComputePass *pass, const char *shaderFile){
    pass->shader = compileShader(shaderFile, "COMPUTE", GL_COMPUTE_SHADER);
    pass->program = glCreateProgram();
    glAttachShader(pass->program, pass->shader);
    glLinkProgram(pass->program);
    glDeleteShader(pass->shader);
    checkProgramCompileErrors(pass->program);
}

void Renderer::linkRaster(core::RasterPass *pass, const char *vertexFile, const char *fragmentFile){
    pass->vertexShader = compileShader(vertexFile, "VERTEX", GL_VERTEX_SHADER);
    pass->fragmentShader = compileShader(fragmentFile, "FRAGMENT", GL_FRAGMENT_SHADER);
    pass->program = glCreateProgram();
    glAttachShader(pass->program, pass->vertexShader);
    glAttachShader(pass->program, pass->fragmentShader);
    glLinkProgram(pass->program);
    glDeleteShader(pass->vertexShader);
    glDeleteShader(pass->fragmentShader);
    checkProgramCompileErrors(pass->program);
}

GLuint Renderer::compileShader(const char* path, std::string type, GLuint gl_type){
    //Read shader from disk
    std::string shaderCodeRaw;
    std::ifstream shaderFile;
    shaderFile.exceptions (std::ifstream::failbit | std::ifstream::badbit);
    try 
    {
        shaderFile.open(path);
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        shaderFile.close();
        shaderCodeRaw = shaderStream.str();
    }
    catch (std::ifstream::failure& e)
    {
        if(config->debuggingEnabled)config->logMessage("[%f] RENDERER::ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ: %s \n", glfwGetTime(), e.what());
    }
    const char* shaderCode = shaderCodeRaw.c_str();

    //compile shader
    GLuint shader = glCreateShader(gl_type);
    glShaderSource(shader, 1, &shaderCode, NULL);
    glCompileShader(shader);
    int success_s;
    char infoLog[1024];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success_s);
    if(success_s) return shader;
    glGetShaderInfoLog(shader, 1024, NULL, infoLog);
    config->logMessage("[%f] RENDERER::SHADER_COMPILATION_ERROR\n %s \n", glfwGetTime(), infoLog);
    return shader;
}

void Renderer::relinkCompute(core::ComputePass *pass, const char *shaderFile){
    pass->shader = compileShader(shaderFile, "COMPUTE", GL_COMPUTE_SHADER);
    GLuint newProgram = glCreateProgram();
    glAttachShader(newProgram, pass->shader);
    glLinkProgram(newProgram);
    checkProgramCompileErrors(newProgram);
    glDeleteProgram(pass->program);
    pass->program = newProgram;
}

void Renderer::relinkRaster(core::RasterPass *pass, const char *vertexFile, const char *fragmentFile){
    pass->vertexShader = compileShader(vertexFile, "VERTEX", GL_VERTEX_SHADER);
    pass->fragmentShader = compileShader(fragmentFile, "FRAGMENT", GL_FRAGMENT_SHADER);
    GLuint newProgram = glCreateProgram();
    glAttachShader(newProgram, pass->vertexShader);
    glAttachShader(newProgram, pass->fragmentShader);
    glLinkProgram(newProgram);
    checkProgramCompileErrors(newProgram);
    glDeleteProgram(pass->program);
    pass->program = newProgram;
}

void Renderer::checkProgramCompileErrors(unsigned int program)
{
    int success;
    char infoLog[1024];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if(success) return;
    glGetProgramInfoLog(program, 1024, NULL, infoLog);
    config->logMessage("[%f] RENDERER::PROGRAM_LINKING_ERROR \n %s \n", glfwGetTime(), infoLog);
}

void Renderer::checkGLError(bool *success_s){
    GLenum error;
    while((error = glGetError()) != GL_NO_ERROR)
    {
        config->logMessage("[%f] RENDERER::OPENGL_ERROR: %u \n", glfwGetTime(), (unsigned int)error);
        *success_s = false;
    }
}
