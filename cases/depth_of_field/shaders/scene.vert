#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inEmission;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec3 outColor;
layout(location = 3) out float outEmission;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;   // xyz 相机位置, w 保留
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 sceneParams;      // x 环境项, yzw 保留
} scene;

void main()
{
    outWorldPosition = inPosition;
    outWorldNormal = inNormal;
    outColor = inColor;
    outEmission = inEmission;
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
}
