#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorld;
layout(location = 2) flat in vec3 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;

// 场景通道：方向光加环境光，同时把视空间法线写进第二张附件供描边使用
void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    outColor = vec4(inColor * (0.30 + 0.70 * diffuse), 1.0);
    outNormal = vec4(normal, 1.0);
}
