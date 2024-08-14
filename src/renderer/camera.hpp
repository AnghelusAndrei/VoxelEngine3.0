#pragma once

#include <glad/glad.h>

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
        float FOV;
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
    float *FOV;

    friend class Renderer;

    protected:
    void setProgram(GLuint program_);
    void UpdateUBO();
    float aspect_ratio;

    private:
    void GenUBO(GLuint program_);
    void freeVRAM();

    GLuint gl_ID;   
    GLuint program;
};