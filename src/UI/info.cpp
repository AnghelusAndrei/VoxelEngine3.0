#include "info.hpp"
#include "algorithm"

Info::Info(const char* name_) : Widget(name_)
{
    AutoScroll = true;
    Clear();
}

void Info::Clear()
{
    Buf.clear();
    LineOffsets.clear();
    LineOffsets.push_back(0);
}

void Info::vAddLog(const char* fmt, va_list args)
{
    int old_size = Buf.size();
    Buf.appendfv(fmt, args);
    for (int new_size = Buf.size(); old_size < new_size; old_size++)
        if (Buf[old_size] == '\n')
            LineOffsets.push_back(old_size + 1);
}

void Info::AddLog(const char* fmt, ...)
{
    va_list args;
    int old_size = Buf.size();
    va_start(args, fmt);
    Buf.appendfv(fmt, args);
    va_end(args);
    for (int new_size = Buf.size(); old_size < new_size; old_size++)
        if (Buf[old_size] == '\n')
            LineOffsets.push_back(old_size + 1);
}

void Info::DrawLog(){
    // Options menu
    if (ImGui::BeginPopup("Options"))
    {
        ImGui::Checkbox("Auto-scroll", &AutoScroll);
        ImGui::EndPopup();
    }

    // Main window
    if (ImGui::Button("Options"))
        ImGui::OpenPopup("Options");
    ImGui::SameLine();
    bool clear = ImGui::Button("Clear");
    ImGui::SameLine();
    bool copy = ImGui::Button("Copy");
    ImGui::SameLine();
    Filter.Draw("Filter", -100.0f);

    ImGui::Separator();

    if (ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar))
    {
        if (clear)
            Clear();
        if (copy)
            ImGui::LogToClipboard();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        const char* buf = Buf.begin();
        const char* buf_end = Buf.end();
        if (Filter.IsActive())
        {
            for (int line_no = 0; line_no < LineOffsets.Size; line_no++)
            {
                const char* line_start = buf + LineOffsets[line_no];
                const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
                if (Filter.PassFilter(line_start, line_end))
                    ImGui::TextUnformatted(line_start, line_end);
            }
        }
        else
        {
            ImGuiListClipper clipper;
            clipper.Begin(LineOffsets.Size);
            while (clipper.Step())
            {
                for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
                {
                    const char* line_start = buf + LineOffsets[line_no];
                    const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
                    ImGui::TextUnformatted(line_start, line_end);
                }
            }
            clipper.End();
        }
        ImGui::PopStyleVar();

        if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void Info::SetProfilerData(core::DebugInfo *data_){
    data = data_;
}

void Info::DrawProfiler(){
    ImGuiTreeNodeFlags flags;

    double avg_ms;

    {
        if (ms_plot.size() >= max_samples) {
            accum_ms-=ms_plot[0];
            ms_plot.erase(ms_plot.begin());
        }
        ms_plot.push_back(data->end_ms - data->start_ms);
        accum_ms+=data->end_ms - data->start_ms;

        avg_ms = accum_ms/ms_plot.size();

        max_ms = *std::max_element(ms_plot.begin(), ms_plot.end());

        ImGui::PlotLines("", ms_plot.data(), ms_plot.size(), 0, nullptr, 0, max_ms, ImVec2(0, 80));
    }

    ImGui::Text("Average FPS: %2f", (1000.0/(avg_ms)));
    ImGui::Text("Average ms/f: %2f", (avg_ms));

    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150,100,200,255));
    flags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::TreeNodeEx("GPU", flags))
    {
        double gpu_avg_ms;
        {
            if (gpu_ms_plot.size() >= max_samples) {
                gpu_accum_ms-=gpu_ms_plot[0];
                gpu_ms_plot.erase(gpu_ms_plot.begin());
            }
            gpu_ms_plot.push_back(data->gpu_end_ms - data->gpu_start_ms);
            gpu_accum_ms+=data->gpu_end_ms - data->gpu_start_ms;

            gpu_avg_ms = gpu_accum_ms/gpu_ms_plot.size();

            ImGui::PlotLines("", gpu_ms_plot.data(), gpu_ms_plot.size(), 0, nullptr, 0, max_ms, ImVec2(0, 80));
        }

        ImGui::Text("%i%%", (int)((gpu_avg_ms / avg_ms) * 100.0));
        ImGui::Text("Average ms/f: %2f", gpu_avg_ms);

        flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;
        char label[128];
        snprintf(label, sizeof(label), "shader recompilation: %.2f", (data->gpu_shaderCompilation_ms - data->gpu_start_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "framebuffer resize: %2f", (data->gpu_framebufferResize_ms - data->gpu_shaderCompilation_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "pass1: %2f", (data->gpu_pass1_ms - data->gpu_framebufferResize_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "pass2: %2f", (data->gpu_pass2_ms - data->gpu_pass1_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "pass3: %2f", (data->gpu_pass3_ms - data->gpu_pass2_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "pass4: %2f", (data->gpu_end_ms - data->gpu_pass3_ms));
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        ImGui::TreePop();
    }  
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,200,150,255));
    flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;
    if (ImGui::TreeNodeEx("CPU", flags))
    {
        double cpu_avg_ms;
        {
            if (cpu_ms_plot.size() >= max_samples) {
                cpu_accum_ms-=cpu_ms_plot[0];
                cpu_ms_plot.erase(cpu_ms_plot.begin());
            }
            cpu_ms_plot.push_back(data->cpu_end_ms - data->cpu_start_ms);
            cpu_accum_ms+=data->cpu_end_ms - data->cpu_start_ms;

            cpu_avg_ms = cpu_accum_ms/cpu_ms_plot.size();

            ImGui::PlotLines("", cpu_ms_plot.data(), cpu_ms_plot.size(), 0, nullptr, 0, max_ms, ImVec2(0, 80));
        }

        ImGui::Text("%i%%", (int)((cpu_avg_ms / avg_ms) * 100.0));
        ImGui::Text("ms/f: %2f", cpu_avg_ms);

        ImGui::TreePop();
    }  
    ImGui::PopStyleColor();
}

void Info::DrawMemUsage(){

}

void Info::DrawSceneData(){

}

void Info::Draw()
{
    if (!ImGui::Begin(name))
    {
        ImGui::End();
        return;
    }

    if(ImGui::CollapsingHeader("profiler", ImGuiTreeNodeFlags_DefaultOpen))
        DrawProfiler();
    if(ImGui::CollapsingHeader("scene data"))
        DrawSceneData();
    if(ImGui::CollapsingHeader("memory usage"))
        DrawMemUsage();
    if(ImGui::CollapsingHeader("logger", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLog();

    ImGui::End();
}