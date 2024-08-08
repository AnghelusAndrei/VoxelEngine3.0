#include "material.hpp"
#define materialSize 64

MaterialPool::MaterialPool(GLuint shaderID_) : shaderID(shaderID_), length(1){
    capacity = 1<<7;


    GLuint material_index = glGetUniformBlockIndex(shaderID, "MaterialUniform");   
    glUniformBlockBinding(shaderID, material_index, 1);
    
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, capacity * materialSize, NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gl_ID);
}

MaterialPool::~MaterialPool(){
    glDeleteBuffers(1, &gl_ID);
}

uint32_t MaterialPool::addMaterial(Material *material){
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, length * materialSize, materialSize, material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    length++;
    return length-1;
}

bool MaterialPool::setMaterial(Material *material, uint32_t index){
    if(index == 0 || index >= length)
        return false;
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, index * materialSize, materialSize, material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return true;
}