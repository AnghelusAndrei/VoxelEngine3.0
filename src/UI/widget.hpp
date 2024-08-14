#pragma once

#include "../renderer/core.hpp"

#include "imgui.h"
#include "./backends/imgui_impl_glfw.h"
#include "./backends/imgui_impl_opengl3.h"


class Widget{
    public:
        Widget();
        Widget(const char* name_);
        virtual void Render();
        virtual void Draw();
        ~Widget();
    protected:
        const char* name;
};