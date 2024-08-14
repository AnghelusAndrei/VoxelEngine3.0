#pragma once

#include "widget.hpp"

#include "../fpcamera.hpp"

class Control : public Widget
{
public:


    Control(const char* name_);

    void SetConfigs(core::RendererConfig *rendererConfig_, core::FrameConfig *frameConfig_, FPCamera::ControllerConfig *fpconfig_, Camera::Config *camconfig_);

    void DrawSceneControl();
    void DrawShaderControl();
    void DrawLightingControl();

    void Draw() override;

private:
    bool c[5] = {false, false, false, false, false};
    core::RendererConfig *rendererConfig;
    core::FrameConfig *frameConfig;
    FPCamera::ControllerConfig *fpconfig;
    Camera::Config *camconfig;
};