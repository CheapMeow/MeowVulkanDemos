#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec2 outUv;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 options;  // x 视图模式, y 保留, z 保留, w 保留
} scene;

void main()
{
    outWorldPosition = inPosition;
    outUv = inUv;
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
}
