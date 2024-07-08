#include "camera.hpp"
#include "iostream"

Camera::Camera(){

}

Camera::Camera(Config *config){
    position = config->position;
    direction = config->direction;
    aspect_ratio = config->aspect_ratio;
    shaderID = config->shaderID;

    delete config;

    GLuint camera_index = glGetUniformBlockIndex(shaderID, "CameraUniform");   
    glUniformBlockBinding(shaderID, camera_index, 0);
    
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, 64, NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, gl_ID);

    UpdateUBO();
}

void Camera::UpdateUBO(){
    UBO ubo{
        .position = glm::vec4(position, 0.0f),
        .cameraPlane = glm::vec4(direction, 0.0f),
        .cameraPlaneRight = glm::vec4(glm::normalize(glm::cross(glm::vec3(0, 1, 0), direction)) * tanf(glm::radians((float)90.0f/2)) * aspect_ratio,0),
        .cameraPlaneUp = glm::vec4(glm::normalize(glm::cross(glm::vec3(ubo.cameraPlaneRight.x, ubo.cameraPlaneRight.y, ubo.cameraPlaneRight.z), direction)) * tanf(glm::radians((float)90.0f/2)),0),
    };

    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, 64, &ubo, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

Camera::~Camera(){
    glDeleteBuffers(1, &gl_ID);
}