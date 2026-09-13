#version 450

// 乒乓通道：把上一张结果采样回来再写出，用来制造反复的渲染通道边界与附件依赖

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;
    vec4 miscParams;   // x 乒乓的亮度步长, yzw 保留
} scene;

layout(set = 0, binding = 4) uniform sampler2D sourceTexture;

void main()
{
    const vec3 color = texture(sourceTexture, inUv).rgb;
    outColor = vec4(color + vec3(scene.miscParams.x), 1.0);
}
