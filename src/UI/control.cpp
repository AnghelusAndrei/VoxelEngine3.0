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
    ImGui::SliderFloat("fpcam speed",       &(fpconfig->speed),       0.0f, 500.0f);
    ImGui::SliderFloat("fpcam sensitivity", &(fpconfig->sensitivity), 0.0f, 3.0f);
}

void Control::DrawShaderControl(){
    static const char* modes[] = {
        "OCTREE", "MATERIAL", "NORMAL", "VERSION", "CLAIM_AGE", "LRU_OCCUPANCY",
        "VIRTUAL", "SHADE", "SHADING", "SAMPLES", "STEPS"
    };
    int mode = (int)frameConfig->renderType;
    if(ImGui::Combo("render mode", &mode, modes, IM_ARRAYSIZE(modes)))
        frameConfig->renderType = (core::RenderType)mode;

    ImGui::SliderInt("normal precision", &frameConfig->normalPrecision, 1, 10);
    ImGui::SliderInt("virtual scale", &frameConfig->virtualScale, 1, 12);

    if(ImGui::Button("Recompile shaders"))
        frameConfig->shaderRecompilation = true;
}

void Control::DrawLightingControl(){
    ImGui::Text("temporal accumulation (EMA sample caps):");
    ImGui::SliderInt("diffuse window",  &frameConfig->emaDiffuse,  1, 1024);
    ImGui::SliderInt("specular window", &frameConfig->emaSpecular, 1, 64);
    ImGui::Text("a-trous specular filter (adaptive by roughness; 0=off):");
    ImGui::SliderInt  ("spec atrous max iters", &frameConfig->atrousIters,  0, 5);
    ImGui::SliderFloat("atrous sigmaN", &frameConfig->atrousSigmaN, 1.0f, 256.0f);
    ImGui::SliderFloat("atrous sigmaP", &frameConfig->atrousSigmaP, 0.25f, 16.0f);
    ImGui::SliderInt  ("stale frames", &frameConfig->staleFrames, 0, 240);
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
