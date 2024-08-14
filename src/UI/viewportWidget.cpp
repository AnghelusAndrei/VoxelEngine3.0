#include "viewportWidget.hpp"

ViewportWidget::ViewportWidget(const char* name_, glm::vec2 size_, glm::ivec2 fbsize_, float aspect_ratio_) : Widget(name_){
    size = size_;
    fbsize = fbsize_;
    avail_size = ImVec2(size.x * (float)fbsize.x, size.y * (float)fbsize.y);
    aspect_ratio = aspect_ratio_;
}

static void AspectRatio(ImGuiSizeCallbackData* data)
{
    float aspect_ratio = *(float*)data->UserData;
    data->DesiredSize.y = (float)(int)(data->DesiredSize.x / aspect_ratio);
}

void ViewportWidget::Draw(){
    ImGui::SetNextWindowSize(ImVec2(size.x * (float)fbsize.x, size.y * (float)fbsize.y), ImGuiCond_Once); 
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, FLT_MAX), AspectRatio, (void*)&aspect_ratio);

    if (!ImGui::Begin(name))
    {
        ImGui::End();
        return;
    }

    avail_size = ImGui::GetContentRegionAvail();
    ImGui::Image((ImTextureID)gl_ID, avail_size, ImVec2(0, 1), ImVec2(1, 0));

    ImGui::End();
}

bool ViewportWidget::getDrawID(GLuint gl_ID_){
    gl_ID = gl_ID_;
}