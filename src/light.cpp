#include "light.hpp"

LightPool::LightPool(Renderer *renderer_) : shaderID(renderer->programID), renderer(renderer_), length(0){
    capacity = 1<<8;

    GLuint light_index = glGetUniformBlockIndex(shaderID, "LightUniform");   
    glUniformBlockBinding(shaderID, light_index, 2);
    
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, capacity * 48, NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 2, gl_ID);

    renderer->setUniformi(length, "lightNum");
}

LightPool::~LightPool(){
    glDeleteBuffers(1, &gl_ID);
}

uint32_t LightPool::addLight(Light *light){
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, length * 48, 48, light);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    length++;
    renderer->setUniformi(length, "lightNum");
    return length-1;
}

bool LightPool::setLight(Light *light, uint32_t index){
    if(index == 0 || index >= length)
        return false;
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, index * 48, 48, light);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return true;
}

void LightPool::removeLight(){
    length--;
    renderer->setUniformi(length, "lightNum");
}