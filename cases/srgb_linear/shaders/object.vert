#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量
    vec4 options;         // x 反照率按 sRGB 解释, y 法线按 sRGB 解释, z 输出编码, w 色调映射
} scene;

void main()
{
    outWorldPosition = inPosition;
    outWorldNormal = inNormal;
    outUv = inUv;
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
}
