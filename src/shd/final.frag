#version 430 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;   // resolveTexture (RGBA32F, linear color)

uniform ivec2 screenResolution;

void main()
{
    FragColor = vec4(texture(screenTexture, TexCoords).rgb, 1.0);
}
