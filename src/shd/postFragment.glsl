#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform int screenX;
uniform int screenY;

const int sampleSize = 20;
const float margainOfError = 1;

void main()
{
    vec2 screenSize = vec2(float(screenX), float(screenY));

    vec4 col = texture(screenTexture, TexCoords).rgba;
    if(col.a * 256.0 < 0.5){
        FragColor = vec4(col.xyz, 1.0);
        return;
    }

    vec3 avg = col.xyz;
    float num = 1;
    float target = col.a * 256.0;
    ivec2 start = ivec2(TexCoords.x * float(screenSize.x) - float(sampleSize/2), TexCoords.y * float(screenSize.y) - float(sampleSize/2));
    ivec2 end = start + sampleSize;
    for(int i = start.x; i <= end.x; i++){
        for(int j = start.y; j <= end.y; j++){
            vec2 coords = vec2(float(i)/screenSize.x, float(j)/screenSize.y);
            vec4 sample = texture(screenTexture, coords).rgba;
            sample.w *= 256.0;
            if(sample.w > target - margainOfError && sample.w < target + margainOfError){
                avg += sample.xyz;
                num++;
            }
        }
    }
    avg /= num;
    FragColor = vec4(avg.xyz, 1.0);
} 