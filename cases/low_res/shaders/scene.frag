#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) flat in vec3 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;
    vec4 miscParams;
} scene;

// 场景通道：方向光加环境光，同时把法线写进第二张附件供上采样加权使用
void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    outColor = vec4(inColor * (0.25 + 0.75 * diffuse), 1.0);
    outNormal = vec4(normal, 1.0);
}
