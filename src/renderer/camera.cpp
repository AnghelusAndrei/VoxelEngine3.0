#include "camera.hpp"

Camera::Camera(){
}

Camera::Camera(Config *config){
    position     = config->position;
    direction    = config->direction;
    aspect_ratio = config->aspect_ratio;
    FOV          = &config->FOV;
}

void Camera::setProgram(GLuint program_){
    program = program_;
    GLuint camera_index = glGetUniformBlockIndex(program, "CameraUniform");
    glUniformBlockBinding(program, camera_index, 0);
}

void Camera::GenUBO(){
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(UBO), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    UpdateUBO();
}

void Camera::UpdateUBO(){
    glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), direction));
    glm::vec3 up    = glm::normalize(glm::cross(right, direction));
    float halfTan   = tanf(glm::radians(*FOV / 2.0f));

    UBO ubo{
        .position        = glm::vec4(position, 0.0f),
        .cameraPlane     = glm::vec4(direction, 0.0f),
        .cameraPlaneRight = glm::vec4(right * halfTan * aspect_ratio, 0.0f),
        .cameraPlaneUp   = glm::vec4(up    * halfTan, 0.0f),
    };

    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(UBO), &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void Camera::freeVRAM(){
    glDeleteBuffers(1, &gl_ID);
}

Camera::~Camera(){
    freeVRAM();
}
