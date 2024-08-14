#include "material.hpp"
#define UBO_SIZE 64

MaterialPool::MaterialPool() : length(1){
    capacity = 1<<7;
}

void MaterialPool::setProgram(GLuint program_){
    program = program_;
    GLuint material_index = glGetUniformBlockIndex(program, "MaterialUniform");   
    glUniformBlockBinding(program, material_index, 1);
}

void MaterialPool::GenUBO(GLuint program_){
    program = program_;
    GLuint material_index = glGetUniformBlockIndex(program, "MaterialUniform");   
    glUniformBlockBinding(program, material_index, 1);
    
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, capacity * UBO_SIZE, NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gl_ID);
}

void MaterialPool::freeVRAM(){
    glDeleteBuffers(1, &gl_ID); 
}

MaterialPool::~MaterialPool(){
    freeVRAM();
}

uint32_t MaterialPool::addMaterial(Material *material){
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, length * UBO_SIZE, UBO_SIZE, material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    length++;
    return length-1;
}

bool MaterialPool::setMaterial(Material *material, uint32_t index){
    if(index == 0 || index >= length)
        return false;
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER, index * UBO_SIZE, UBO_SIZE, material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return true;
}