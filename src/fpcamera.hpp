#pragma once

#include "./renderer/camera.hpp"

class FPCamera : public Camera{
    public:
    struct KeyMap{
        uint16_t up;
        uint16_t down;
        uint16_t forward;
        uint16_t back;
        uint16_t left;
        uint16_t right;
        uint16_t activation;
    } *keyMap;

    struct ControllerConfig{
        float speed;
        float sensitivity;
        glm::vec2 rotation = glm::vec2(0,0);
    };

    public:

    explicit FPCamera();
    explicit FPCamera(Camera::Config *config_, ControllerConfig *controllerConfig);
    ~FPCamera();
    bool GLFWInput(GLFWwindow* window);

    void setKeyMap(FPCamera::KeyMap *newMap);

    ControllerConfig *config;

    bool firstFrame = true;
    private:

    void defaultKeyMap();

    glm::dvec2 start_angle;
    glm::vec2 rotation;
    double time;
};