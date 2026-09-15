#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

layout(set = 0, binding = 0) uniform ToneMapBuffer {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 sunDirection;
    vec4 skyParams;
    vec4 operatorParams;
    vec4 encodingParams;
} tm;

void main()
{
    outWorldPosition = inPosition;
    outWorldNormal = inNormal;
    outUv = inUv;
    gl_Position = tm.viewProjection * vec4(inPosition, 1.0);
}
