#pragma once
#include "widget.hpp"

class Interface{
    public:
        Interface(GLFWwindow *window_, const char* glsl_version_);
        void Draw(Widget *widgets[], uint32_t widgetsNum);
        void Render();
        ~Interface();

        ImGuiIO& io;

    private:
        const char* glsl_version;
        GLFWwindow *window;
};