#include "control.hpp"

Control::Control(const char* name_){
    name = name_;
}

void Control::SetConfigs(core::RendererConfig *rendererConfig_, core::FrameConfig *frameConfig_, FPCamera::ControllerConfig *fpconfig_, Camera::Config *camconfig_){
    rendererConfig = rendererConfig_;
    frameConfig = frameConfig_;
    fpconfig = fpconfig_;
    camconfig = camconfig_;
}

void Control::DrawSceneControl(){
    ImGui::SliderFloat("fpcam speed", &(fpconfig->speed), 0.0f, 500.0f);
    ImGui::SliderFloat("fpcam sensitivity", &(fpconfig->sensitivity), 0.0f, 3.0f);
}

void Control::DrawShaderControl(){
    ImGui::Text("View Types: ");

    bool DEFAULT_b = frameConfig->renderType == core::RenderType::DEFAULT;
    bool STRUCTURE_b = frameConfig->renderType == core::RenderType::STRUCTURE;
    bool ALBEDO_b = frameConfig->renderType == core::RenderType::ALBEDO;
    bool NORMAL_b = frameConfig->renderType == core::RenderType::NORMAL;
    bool VOXELID_b = frameConfig->renderType == core::RenderType::VOXELID;

    if (ImGui::Checkbox("DEFAULT", &DEFAULT_b))
    {
        if (DEFAULT_b) frameConfig->renderType = core::RenderType::DEFAULT;
        else frameConfig->renderType = core::RenderType::DEFAULT;
    }

    if (ImGui::Checkbox("STRUCTURE", &STRUCTURE_b))
    {
        if (STRUCTURE_b) frameConfig->renderType = core::RenderType::STRUCTURE;
        else frameConfig->renderType = core::RenderType::STRUCTURE;
    }

    if (ImGui::Checkbox("ALBEDO", &ALBEDO_b))
    {
        if (ALBEDO_b) frameConfig->renderType = core::RenderType::ALBEDO;
        else frameConfig->renderType = core::RenderType::ALBEDO;
    }

    if (ImGui::Checkbox("NORMAL", &NORMAL_b))
    {
        if (NORMAL_b) frameConfig->renderType = core::RenderType::NORMAL;
        else frameConfig->renderType = core::RenderType::NORMAL;
    }

    if (ImGui::Checkbox("VOXELID", &VOXELID_b))
    {
        if (VOXELID_b) frameConfig->renderType = core::RenderType::VOXELID;
        else frameConfig->renderType = core::RenderType::VOXELID;
    }

    // Reset all other options when one is selected
    if (DEFAULT_b) STRUCTURE_b = ALBEDO_b = NORMAL_b = VOXELID_b = false;
    if (STRUCTURE_b) DEFAULT_b = ALBEDO_b = NORMAL_b = VOXELID_b = false;
    if (ALBEDO_b) DEFAULT_b = STRUCTURE_b = NORMAL_b = VOXELID_b = false;
    if (NORMAL_b) DEFAULT_b = STRUCTURE_b = ALBEDO_b = VOXELID_b = false;
    if (VOXELID_b) DEFAULT_b = STRUCTURE_b = ALBEDO_b = NORMAL_b = false;

    ImGui::Separator();
    if(ImGui::Button("Recompile shaders"))
        frameConfig->shaderRecompilation = true;
}

void Control::DrawLightingControl(){
    ImGui::Text("pipeline specific:");
    ImGui::SliderFloat("lBufferSwapSec", &(frameConfig->lBufferSwapSeconds), 0.0f, 0.5f);
    ImGui::Separator();
    ImGui::Text("raytracing:");
    ImGui::SliderInt("spp", &(frameConfig->spp), 1, 10);
    ImGui::SliderInt("bounces", &(frameConfig->bounces), 1, 10);
    ImGui::SliderInt("max checks", &(frameConfig->controlchecks), 1, 300);
}

void Control::Draw(){
    if(!ImGui::Begin(name)){
        ImGui::End();
        return;
    }

    if(ImGui::CollapsingHeader("scene", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSceneControl();
    if(ImGui::CollapsingHeader("shaders", ImGuiTreeNodeFlags_DefaultOpen))
        DrawShaderControl();
    if(ImGui::CollapsingHeader("lighting", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLightingControl();

    ImGui::End();
}