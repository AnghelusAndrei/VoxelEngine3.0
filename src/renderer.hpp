#pragma once
#include <iostream>
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

#include <thread>

#include "config.hpp"


class Renderer{
    public:
    Renderer(const Config *config);
    void run();
    ~Renderer();

    GLFWwindow* window;
    private:
    
    bool InitImGUI();
    static void glfw_error_callback(int error, const char* description);
};