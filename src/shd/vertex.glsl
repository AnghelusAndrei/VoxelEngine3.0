#version 330 core
layout (location = 0) in vec3 aPos;

out vec4 vertexPosition;

void main()
{
    gl_Position = vec4(aPos.xyz, 1.0);
    vertexPosition = vec4(aPos.x > 0 ? 1 : -1, aPos.y, aPos.z, 1.0);
}