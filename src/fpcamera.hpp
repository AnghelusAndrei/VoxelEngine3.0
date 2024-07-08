#pragma once

#include "camera.hpp"

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
    };

    public:

    explicit FPCamera();
    explicit FPCamera(Camera::Config *config_, ControllerConfig *controllerConfig);
    ~FPCamera();
    void GLFWInput(GLFWwindow* window);

    void setKeyMap(FPCamera::KeyMap *newMap);

    ControllerConfig *config;
    private:

    void defaultKeyMap();

    glm::dvec2 start_angle;
    glm::vec2 rotation;
    bool firstFrame = true;
    double time;
};