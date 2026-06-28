#pragma once

#include "widget.hpp"
#include "../renderer/profiler.hpp"

class Info : public Widget
{
public:

    //log
    ImGuiTextBuffer     Buf;
    ImGuiTextFilter     Filter;
    ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
    bool                AutoScroll;  // Keep scrolling if already at the bottom.

    Info(const char* name_);

    //log
    void Clear();
    void vAddLog(const char* fmt, va_list args) ;
    void AddLog(const char* fmt, ...) ;
    void DrawLog();

    const Profiler        *profiler_ = nullptr;
    const core::FrameStats *stats_   = nullptr;

    std::vector<float> ms_plot;
    std::vector<float> gpu_ms_plot;
    std::vector<float> cpu_ms_plot;
    double accum_ms = 0;
    double gpu_accum_ms = 0;
    double cpu_accum_ms = 0;
    double max_ms = 0;
    const int max_samples = 200;

    void setData(const Profiler *profiler, const core::FrameStats *stats);
    void DrawProfiler();

    void DrawMemUsage();

    void DrawSceneData();

    void Draw() override;
};