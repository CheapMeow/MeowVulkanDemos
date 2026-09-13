#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 modeParams;       // x 描边方式, y 深度通道开关, z 法线通道开关, w 外扩距离
    vec4 outlineParams;    // x 阈值, y 线宽, z 是否按屏幕距离缩放, w 保留
} scene;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;
layout(set = 0, binding = 4) uniform sampler2D sceneColor;

vec3 tonemap(vec3 color)
{
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

// 后处理描边：靠深度与法线的不连续找边缘，没有额外几何也没有额外绘制命令
void main()
{
    const vec2 texel = scene.outlineParams.y / vec2(textureSize(sceneColor, 0));
    const float centerDepth = texture(sceneDepth, inUv).r;
    const vec3 centerNormal = texture(sceneNormal, inUv).xyz;

    float edge = 0.0;
    const vec2 offsets[4] = vec2[4](vec2(texel.x, 0.0), vec2(-texel.x, 0.0), vec2(0.0, texel.y),
                                    vec2(0.0, -texel.y));
    for (int i = 0; i < 4; ++i) {
        const vec2 sampleUv = inUv + offsets[i];
        const float depth = texture(sceneDepth, sampleUv).r;
        const vec3 normal = texture(sceneNormal, sampleUv).xyz;

        if (scene.modeParams.y > 0.5 &&
            abs(depth - centerDepth) > scene.outlineParams.x * max(centerDepth, 1e-4)) {
            edge = 1.0;
        }
        if (scene.modeParams.z > 0.5 && dot(normal, centerNormal) < 1.0 - scene.outlineParams.x) {
            edge = 1.0;
        }
    }

    const vec3 color = texture(sceneColor, inUv).rgb;
    outColor = vec4(tonemap(mix(color, vec3(0.95, 0.85, 0.25) * 4.0, edge)), 1.0);
}
