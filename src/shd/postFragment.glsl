#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform int screenX;
uniform int screenY;

void main()
{
    vec2 screenSize = vec2(float(screenX), float(screenY));

    vec4 col = texture(screenTexture, TexCoords).rgba;
    FragColor = vec4(col.xyz, 1.0);
} 