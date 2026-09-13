#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;
    vec4 colorParams;
    vec4 modeParams;
    vec4 layerParams;
} scene;

layout(set = 0, binding = 2) uniform sampler2D accumColor;
layout(set = 0, binding = 3) uniform sampler2D revealage;

layout(set = 0, binding = 7) buffer LayerCountBuffer {
    uint counts[];
} layerCount;

vec3 heatColor(uint count)
{
    float t = clamp(float(count) / float(scene.layerParams.x), 0.0, 1.0);
    vec3 low = vec3(0.05, 0.08, 0.30);
    vec3 mid = vec3(0.10, 0.75, 0.35);
    vec3 high = vec3(1.00, 0.85, 0.20);
    return t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, t * 2.0 - 1.0);
}

// 加权混合的结果：累积颜色除以累积权重，再按露出度合成到背景上
void main()
{
    vec4 accum = texture(accumColor, inUv);
    float reveal = texture(revealage, inUv).r;
    vec3 color = accum.rgb / max(accum.a, 1e-4);
    float alpha = 1.0 - reveal;

    vec3 result = mix(scene.colorParams.rgb, color, clamp(alpha, 0.0, 1.0));
    if (scene.modeParams.y > 0.5) {
        const uint pixel = uint(gl_FragCoord.y) * uint(scene.viewportParams.x) + uint(gl_FragCoord.x);
        result = heatColor(layerCount.counts[pixel]);
    }
    outColor = vec4(result, 1.0);
}
