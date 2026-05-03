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

void Info::setData(const Profiler *profiler, const core::FrameStats *stats) {
    profiler_ = profiler;
    stats_    = stats;
}

void Info::DrawProfiler(){
    const ProfilerResults *pr = profiler_ ? profiler_->getResults() : nullptr;
    if (!pr) {
        ImGui::TextDisabled("Waiting for profiler data...");
        return;
    }

    ImGuiTreeNodeFlags flags;
    double avg_ms;

    // ---- Frame time plot (CPU wall clock) -----------------------------------
    {
        if (ms_plot.size() >= (size_t)max_samples) {
            accum_ms -= ms_plot[0];
            ms_plot.erase(ms_plot.begin());
        }
        ms_plot.push_back((float)pr->CPU_ms);
        accum_ms += pr->CPU_ms;

        avg_ms = accum_ms / ms_plot.size();
        max_ms = *std::max_element(ms_plot.begin(), ms_plot.end());

        ImGui::PlotLines("", ms_plot.data(), (int)ms_plot.size(), 0, nullptr, 0.0f,
                         (float)max_ms, ImVec2(0, 80));
    }

    ImGui::Text("Average FPS: %.2f", 1000.0 / avg_ms);
    ImGui::Text("Average ms/f: %.2f", avg_ms);

    ImGui::Separator();

    // ---- GPU section --------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150,100,200,255));
    flags = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::TreeNodeEx("GPU", flags))
    {
        double gpu_avg_ms;
        {
            if (gpu_ms_plot.size() >= (size_t)max_samples) {
                gpu_accum_ms -= gpu_ms_plot[0];
                gpu_ms_plot.erase(gpu_ms_plot.begin());
            }
            gpu_ms_plot.push_back((float)pr->GPU_ms);
            gpu_accum_ms += pr->GPU_ms;
            gpu_avg_ms = gpu_accum_ms / gpu_ms_plot.size();

            ImGui::PlotLines("", gpu_ms_plot.data(), (int)gpu_ms_plot.size(), 0, nullptr, 0.0f,
                             (float)max_ms, ImVec2(0, 80));
        }

        ImGui::Text("%i%%", (int)((gpu_avg_ms / avg_ms) * 100.0));
        ImGui::Text("Average ms/f: %.2f", gpu_avg_ms);

        flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;
        char label[128];

        // Wavefront stage timings (see renderer.cpp::runWavefrontFrame for the
        // dispatch ordering).
        snprintf(label, sizeof(label), "primary (GBuffer): %.3f ms", pr->primary_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "normal: %.3f ms", pr->normal_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "schedule: %.3f ms", pr->schedule_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "schedule readback: %.3f ms", pr->scheduleReadback_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "rank: %.3f ms", pr->rank_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "rank readback (shadeCount): %.3f ms", pr->rankReadback_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        // threshold.comp scans the histogram rank.comp built and writes
        // tHigh/tMid into rankStatsBuf — replaces the old CPU readback of
        // min/max stats. Should run in microseconds.
        snprintf(label, sizeof(label), "threshold (GPU scan): %.3f ms", pr->threshold_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "chunk loop: %.3f ms", pr->chunkLoop_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        if (ImGui::CollapsingHeader("per-chunk timings")) {
            for (int i = 0; i < pr->numChunks; i++) {
                const ChunkResult& ck = pr->chunks[i];

                snprintf(label, sizeof(label), "chunk %i emit: %.3f ms", i, ck.emit_ms);
                if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

                snprintf(label, sizeof(label), "chunk %i bounces: %.3f ms", i, ck.bounces_ms);
                if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

                // Unique CollapsingHeader ID per chunk to avoid ImGui ID collision.
                snprintf(label, sizeof(label), "per-bounce timings##chunk%i", i);
                if (ImGui::CollapsingHeader(label)) {
                    for (int j = 0; j < ck.numBounces; j++) {
                        snprintf(label, sizeof(label), "bounce %i trace: %.3f ms", j, ck.bounces[j].trace_ms);
                        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();
                        snprintf(label, sizeof(label), "bounce %i shade: %.3f ms", j, ck.bounces[j].shade_ms);
                        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();
                    }
                }
            }
        }

        snprintf(label, sizeof(label), "resolve: %.3f ms", pr->resolve_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        snprintf(label, sizeof(label), "final blit: %.3f ms", pr->finalBlit_ms);
        if (ImGui::TreeNodeEx(label, flags)) ImGui::TreePop();

        ImGui::TreePop();
    }
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ---- CPU section --------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200,200,150,255));
    flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;
    if (ImGui::TreeNodeEx("CPU", flags))
    {
        double cpu_avg_ms;
        {
            double cpu_ms = pr->CPU_ms - pr->GPU_ms;
            if (cpu_ms_plot.size() >= (size_t)max_samples) {
                cpu_accum_ms -= cpu_ms_plot[0];
                cpu_ms_plot.erase(cpu_ms_plot.begin());
            }
            cpu_ms_plot.push_back((float)cpu_ms);
            cpu_accum_ms += cpu_ms;
            cpu_avg_ms = cpu_accum_ms / cpu_ms_plot.size();

            ImGui::PlotLines("", cpu_ms_plot.data(), (int)cpu_ms_plot.size(), 0, nullptr, 0.0f,
                             (float)max_ms, ImVec2(0, 80));
        }

        ImGui::Text("%i%%", (int)((cpu_avg_ms / avg_ms) * 100.0));
        ImGui::Text("ms/f: %.2f", cpu_avg_ms);

        ImGui::TreePop();
    }
    ImGui::PopStyleColor();
}

void Info::DrawMemUsage(){
    if (!stats_) { ImGui::TextDisabled("No stats available."); return; }
    constexpr double MB = 1000.0 * 1000.0;
    double total_mem = (double)(stats_->scene_mem + stats_->lBuffer_mem + stats_->nBuffer_mem
                                + stats_->rBuffer_mem + stats_->rayRing_mem + stats_->claimMap_mem) / MB;
    ImGui::Text("total memory allocated: %.3f mb", total_mem);
    ImGui::Text("volume memory: %.3f mb",    (double)stats_->scene_mem    / MB);
    ImGui::Text("volume capacity: %.3f mb",  (double)stats_->scene_capacity / MB);
    ImGui::Text("lBuffer memory: %.3f mb",   (double)stats_->lBuffer_mem  / MB);
    ImGui::Text("rBuffer memory: %.3f mb",   (double)stats_->rBuffer_mem  / MB);
    ImGui::Text("nBuffer memory: %.3f mb",   (double)stats_->nBuffer_mem  / MB);
    ImGui::Text("ray ring memory: %.3f mb",  (double)stats_->rayRing_mem  / MB);
    ImGui::Text("shade list memory: %.3f mb",(double)stats_->shadeList_mem / MB);
    ImGui::Text("claim map memory: %.3f mb", (double)stats_->claimMap_mem / MB);
}

void Info::DrawSceneData(){
    if (!stats_) { ImGui::TextDisabled("No stats available."); return; }
    ImGui::Text("voxels in view: %u",     stats_->scheduledCount);
    ImGui::Text("max voxels in view: %u", stats_->shadingBudget);
    ImGui::Text("num voxels: %u",         stats_->voxels_num);
    ImGui::Text("cam position:  \n     x:%f \n     y:%f \n     z:%f",
                stats_->cam_position.x, stats_->cam_position.y, stats_->cam_position.z);
    ImGui::Text("cam direction: \n     x:%f \n     y:%f \n     z:%f",
                stats_->cam_direction.x, stats_->cam_direction.y, stats_->cam_direction.z);
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
    if(ImGui::CollapsingHeader("scene data", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSceneData();
    if(ImGui::CollapsingHeader("memory usage", ImGuiTreeNodeFlags_DefaultOpen))
        DrawMemUsage();
    if(ImGui::CollapsingHeader("logger", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLog();

    ImGui::End();
}