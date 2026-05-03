#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;

uniform ivec2 screenResolution;

// Or ACES Filmic — better color in highlights, ~5 ops
vec3 aces(vec3 x) {
    return clamp((x*(2.51*x + 0.03)) / (x*(2.43*x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec4 col = texture(screenTexture, TexCoords).rgba;
    FragColor = vec4(aces(col.xyz), 1.0);
} 