#pragma once
#include "imgui.h"
#include "./backends/imgui_impl_glfw.h"
#include "./backends/imgui_impl_opengl3.h"

class Log
{
public:

    ImGuiTextBuffer     Buf;
    ImGuiTextFilter     Filter;
    ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
    bool                AutoScroll;  // Keep scrolling if already at the bottom.

    Log();

    void Clear();
    void AddLog(const char* fmt, ...) ;
    void Draw(const char* title, bool* p_open = NULL);
};