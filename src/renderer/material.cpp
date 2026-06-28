#include "material.hpp"

static_assert(sizeof(Material) == 64,
    "Material must be exactly 64 bytes to match GLSL std140 array stride");

MaterialPool::MaterialPool() : length(1){
    capacity = 1<<7;
    materials.resize(1);   // index 0 = empty sentinel (matches `length` starting at 1)
}

void MaterialPool::setProgram(GLuint program_){
    program = program_;
    GLuint material_index = glGetUniformBlockIndex(program, "MaterialUniform");
    glUniformBlockBinding(program, material_index, 1);
}

void MaterialPool::GenUBO(){
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferData(GL_UNIFORM_BUFFER, capacity * (GLsizeiptr)sizeof(Material), NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void MaterialPool::freeVRAM(){
    glDeleteBuffers(1, &gl_ID);
}

MaterialPool::~MaterialPool(){
    freeVRAM();
}

uint32_t MaterialPool::addMaterial(Material *material){
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER,
                    (GLintptr)(length * sizeof(Material)),
                    (GLsizeiptr)sizeof(Material),
                    material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    if (materials.size() <= length) materials.resize(length + 1);
    materials[length] = *material;          // mirror on the CPU
    length++;
    return length-1;
}

bool MaterialPool::setMaterial(Material *material, uint32_t index){
    if(index == 0 || index >= length)
        return false;
    glBindBuffer(GL_UNIFORM_BUFFER, gl_ID);
    glBufferSubData(GL_UNIFORM_BUFFER,
                    (GLintptr)(index * sizeof(Material)),
                    (GLsizeiptr)sizeof(Material),
                    material);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    if (materials.size() <= index) materials.resize(index + 1);
    materials[index] = *material;           // mirror on the CPU
    return true;
}
