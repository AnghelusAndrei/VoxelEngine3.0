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
    // Render-type selector. RadioButton is the right widget for a one-of-N
    // pick — using Checkbox here was the source of the old bug where each
    // box's "uncheck" branch re-set the same value, leaving the user unable
    // to switch back to DEFAULT once any other mode was selected. resolve.comp
    // reads the chosen mode via its renderType uniform; no recompile needed.
    ImGui::Text("View Type:");
    int rt = (int)frameConfig->renderType;
    ImGui::RadioButton("DEFAULT (lit)",   &rt, (int)core::RenderType::DEFAULT);
    ImGui::RadioButton("STRUCTURE",       &rt, (int)core::RenderType::STRUCTURE);
    ImGui::RadioButton("ALBEDO",          &rt, (int)core::RenderType::ALBEDO);
    ImGui::RadioButton("NORMAL",          &rt, (int)core::RenderType::NORMAL);
    ImGui::RadioButton("VOXELID",         &rt, (int)core::RenderType::VOXELID);
    ImGui::RadioButton("VARIANCE",        &rt, (int)core::RenderType::VARIANCE);
    frameConfig->renderType = (core::RenderType)rt;

    ImGui::Separator();
    if(ImGui::Button("Recompile shaders"))
        frameConfig->shaderRecompilation = true;
}

void Control::DrawLightingControl(){
    // Wavefront path tracer is now the only renderer. spp / bounces are
    // controlled by the wavefront pipeline itself (shadingBudget caps the
    // primary rays per frame; MAX_BOUNCES in wavefront.glsl caps depth).
    ImGui::Text("traversal:");
    ImGui::SliderInt("primary checks", &(frameConfig->primary_controlchecks), 1, 200);
    ImGui::SliderInt("bounce checks",  &(frameConfig->bounce_controlchecks),  1, 400);
    ImGui::SliderInt("recon radius",   &(frameConfig->reconstructionRadius),  0, 30);

    ImGui::Separator();
    ImGui::Text("budget:");
    ImGui::SliderInt("shading budget", &(frameConfig->shadingBudget), 1024, 196608);

    ImGui::Separator();
    // ---- Importance-aware ranking (rank.comp + threshold.comp on GPU) ------
    // tHigh/tMid are no longer host-side knobs — threshold.comp derives them
    // from a 256-bin histogram each frame. The fractions below tell that pass
    // what slice of the distribution to put in each tier. (top + mid) <= 1.
    // weightPixels + weightVariance combine into the importance score; pixels
    // saturates at pixelsNorm and indirect-channel variance at varianceNorm.
    ImGui::Text("ranking (importance tiers):");
    ImGui::SliderFloat("top fraction (2 spv)", &(frameConfig->topFraction),    0.0f, 1.0f);
    ImGui::SliderFloat("mid fraction (1 spv)", &(frameConfig->midFraction),    0.0f, 1.0f);
    ImGui::SliderInt  ("min samples (floor)",  &(frameConfig->minSamples),     0,    2);
    ImGui::SliderFloat("weight pixels",        &(frameConfig->weightPixels),   0.0f, 1.0f);
    ImGui::SliderFloat("weight variance",      &(frameConfig->weightVariance), 0.0f, 1.0f);
    ImGui::SliderFloat("pixels saturate at",   &(frameConfig->pixelsNorm),     1.0f,  2000.0f);
    ImGui::SliderFloat("variance saturate at", &(frameConfig->varianceNorm),   100.0f, 100000.0f);

    ImGui::Separator();
    // ---- Firefly clamp (shade.comp::depositSampleDual) ---------------------
    // Per-channel relative luma clamp on incoming samples. Eliminates the
    // bright-stuck-voxel artifact from single low-PDF NEE / RIS spikes.
    ImGui::Text("firefly clamp (lBuffer deposit):");
    ImGui::SliderFloat("firefly K (mean ×)",        &(frameConfig->fireflyK),     1.0f, 16.0f);
    ImGui::SliderFloat("firefly floor (urgb)",      &(frameConfig->fireflyFloor), 0.0f, 255.0f);
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