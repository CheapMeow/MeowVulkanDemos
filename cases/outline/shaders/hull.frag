#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorld;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;
    vec4 outlineParams;
} scene;

void main()
{
    // 外扩出去的那一层只写描边颜色，法线附件留空，不参与后处理描边的边缘判定
    outColor = vec4(0.95, 0.85, 0.25, 1.0);
    outNormal = vec4(0.0, 0.0, 1.0, 1.0);
}
