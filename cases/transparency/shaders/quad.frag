#version 450

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;
    vec4 colorParams;
    vec4 modeParams;
    vec4 layerParams;
} scene;

layout(set = 0, binding = 2) buffer LayerCountBuffer {
    uint counts[];
} layerCount;

// 源混合：保留 alpha 交给固定功能混合，绘制顺序决定结果
void main()
{
    if (scene.modeParams.y > 0.5) {
        const uint pixel = uint(gl_FragCoord.y) * uint(scene.viewportParams.x) + uint(gl_FragCoord.x);
        atomicAdd(layerCount.counts[pixel], 1u);
    }
    outColor = inColor;
}
