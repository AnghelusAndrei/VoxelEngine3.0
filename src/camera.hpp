#pragma once

#include <glad/glad.h>

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/rotate_vector.hpp>

class Camera{
    public:
    struct Config{
        glm::vec3 position; 
        glm::vec3 direction; 
        float aspect_ratio;
        uint32_t shaderID;
    };

    struct UBO{
        glm::vec4 position;
        glm::vec4 cameraPlane;
        glm::vec4 cameraPlaneRight;
        glm::vec4 cameraPlaneUp;
    };

    public:
    explicit Camera();
    explicit Camera(Config *config);
    ~Camera();

    glm::vec3 position;
    glm::vec3 direction;

    protected:
    void UpdateUBO();
    float aspect_ratio;

    private:

    GLuint gl_ID;   
    GLuint shaderID;
};