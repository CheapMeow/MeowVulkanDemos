#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;
    vec4 outlineParams;
} scene;

layout(set = 0, binding = 4) uniform sampler2D sceneColor;

vec3 tonemap(vec3 color)
{
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

// 不描边时的输出，以及双 Pass 外扩的结果（描边已经在颜色附件里了）
void main()
{
    outColor = vec4(tonemap(texture(sceneColor, inUv).rgb), 1.0);
}
