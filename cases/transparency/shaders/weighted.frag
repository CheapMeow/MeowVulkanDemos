#version 450

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outAccum;
layout(location = 1) out vec4 outReveal;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;
    vec4 colorParams;
    vec4 modeParams;
    vec4 layerParams;
} scene;

layout(set = 0, binding = 2) buffer LayerCountBuffer {
    uint counts[];
} layerCount;

// 加权混合：一遍累积颜色与权重，再累积露出度，不需要排序
void main()
{
    if (scene.modeParams.y > 0.5) {
        const uint pixel = uint(gl_FragCoord.y) * uint(scene.viewportParams.x) + uint(gl_FragCoord.x);
        atomicAdd(layerCount.counts[pixel], 1u);
    }

    const float alpha = inColor.a;
    // 权重随深度下降，近处的色片占更大比重，这是加权混合的经典近似
    const float depth = clamp(gl_FragCoord.z, 0.0, 1.0);
    const float weight = alpha * clamp(1.0 / (1e-4 + depth * depth), 0.01, 100.0);
    outAccum = vec4(inColor.rgb * alpha * weight, alpha * weight);
    outReveal = vec4(alpha, 0.0, 0.0, 1.0);
}
