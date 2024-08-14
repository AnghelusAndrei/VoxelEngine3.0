#pragma once

#include "widget.hpp"

class Logger : public Widget
{
public:

    ImGuiTextBuffer     Buf;
    ImGuiTextFilter     Filter;
    ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
    bool                AutoScroll;  // Keep scrolling if already at the bottom.

    Logger(const char* name_);

    void Clear();
    void vAddLog(const char* fmt, va_list args) ;
    void AddLog(const char* fmt, ...) ;
    void Draw() override;
};