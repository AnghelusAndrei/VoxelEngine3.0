#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <glad/glad.h>
#include "imgui.h"
#include "./backends/imgui_impl_glfw.h"
#include "./backends/imgui_impl_opengl3.h"
#include <stdio.h>
#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <chrono>

#include "config.hpp"
#include "uiconfig.hpp"

#include "fpcamera.hpp"
#include "material.hpp"

#include "Logger/logger.hpp"


class Renderer{
    public:
    Renderer(const Config *config);
    void run();
    ~Renderer();

    UIConfig* ui;
    GLFWwindow* window;
    GLuint programID, vertex, fragment;
    GLuint postProgramID, postVertex, postFragment;

    Log *log;

    void static glfw_error_callback(int error, const char* description);
    private:
    
    ImVec4 clear_color;
    float aspect_ratio;
    glm::ivec2 display_size;
    void InitImGUI(const char* glsl_version);
    void ImGUIpass();

    void checkShaderCompileErrors(unsigned int shader, std::string type);

    GLuint compileShader(const char* path, std::string type, GLuint gl_type);
    GLuint VBO, VAO;
    GLuint framebuffer, textureColorbuffer, rbo;
    GLuint quadVAO, quadVBO;
    std::vector<GLuint> textureIDs;

    public:
    void addTexture(GLuint texID);

    void setUniformi(int v, std::string name);
    void setUniformui(unsigned int v, std::string name);
    void setUniformf(float v, std::string name);

    private:
    std::vector<std::string> uniformiNames;
    std::vector<std::string> uniformuiNames;
    std::vector<std::string> uniformfNames;

    std::vector<int> uniformiValues;
    std::vector<unsigned int> uniformuiValues;
    std::vector<float> uniformfValues;
};