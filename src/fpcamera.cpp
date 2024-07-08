#include "fpcamera.hpp"
#include "iostream"

FPCamera::FPCamera() : Camera(){
    defaultKeyMap();
}

FPCamera::FPCamera(Camera::Config *config_, ControllerConfig *controllerConfig) : Camera(config_){
    defaultKeyMap();
    config = controllerConfig;
}

FPCamera::~FPCamera(){
    delete config;
}

void FPCamera::defaultKeyMap(){
    keyMap = new KeyMap{
        .up = GLFW_KEY_SPACE,
        .down = GLFW_KEY_LEFT_SHIFT,
        .forward = GLFW_KEY_W,
        .back = GLFW_KEY_S,
        .left = GLFW_KEY_A,
        .right = GLFW_KEY_D,
        .activation = GLFW_PRESS,
    };
}


void FPCamera::setKeyMap(FPCamera::KeyMap *newMap){
    keyMap = newMap;
}

void FPCamera::GLFWInput(GLFWwindow* window){
    double deltaTime = glfwGetTime() - time;
    time = glfwGetTime();

    glm::vec3 up = glm::vec3(0,1,0);
    glm::vec2 dir2d = glm::normalize(glm::vec2(direction.x,direction.z));
    glm::vec3 dir3d = glm::vec3(dir2d.x,0,dir2d.y);

    if(glfwGetKey(window, keyMap->down) == keyMap->activation)
        position -= (float)(deltaTime*config->speed) * up;
    
    if(glfwGetKey(window, keyMap->up) == keyMap->activation)
        position += (float)(deltaTime*config->speed) * up;

    if(glfwGetKey(window, keyMap->forward) == keyMap->activation)
        position += (float)(deltaTime*config->speed) * dir3d;

    if(glfwGetKey(window, keyMap->back) == keyMap->activation)
        position -= (float)(deltaTime*config->speed) * dir3d;

    if(glfwGetKey(window, keyMap->left) == keyMap->activation)
        position += (float)(deltaTime*config->speed) * glm::normalize(glm::cross(dir3d, up));

    if(glfwGetKey(window, keyMap->right) == keyMap->activation)
        position += (float)(deltaTime*config->speed) * glm::normalize(glm::cross(up, dir3d));


    static glm::dvec2 mouse;
    glfwGetCursorPos(window, &mouse.x, &mouse.y);
    
    if(firstFrame){
        firstFrame = false;
        start_angle = glm::dvec2(mouse);
        rotation = glm::vec2(0,0);
    }else{
        rotation.x = fmod(rotation.x - (mouse.x - start_angle.x)*config->sensitivity, 360);
        rotation.y = glm::clamp(rotation.y - (mouse.y - start_angle.y)*config->sensitivity, -89.0, 89.0);

        direction.x = cosf(glm::radians(rotation.x))*cosf(glm::radians(rotation.y));
        direction.y = sinf(glm::radians(rotation.y));
        direction.z = sinf(glm::radians(rotation.x))*cosf(glm::radians(rotation.y));

        start_angle = glm::dvec2(mouse);
    }

    UpdateUBO();
}