#version 450

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outDummy;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;
    vec4 colorParams;
    vec4 modeParams;
    vec4 layerParams;
} scene;

layout(set = 0, binding = 2) buffer LayerCountBuffer {
    uint counts[];
} layerCount;

layout(set = 0, binding = 3) buffer HeadBuffer {
    uint heads[];
} headBuffer;

layout(set = 0, binding = 4) buffer NodeColorBuffer {
    vec4 colors[];
} nodeColors;

layout(set = 0, binding = 5) buffer NodeMetaBuffer {
    uvec2 metas[];  // x = 深度位, y = 下一个节点
} nodeMeta;

layout(set = 0, binding = 6) buffer NodeCounterBuffer {
    uint count;
} nodeCounter;

// 逐像素链表：一次原子自增分配节点，再把节点插到本像素的链表头
void main()
{
    const uint pixel = uint(gl_FragCoord.y) * uint(scene.viewportParams.x) + uint(gl_FragCoord.x);
    if (scene.modeParams.y > 0.5) {
        atomicAdd(layerCount.counts[pixel], 1u);
    }

    const uint index = atomicAdd(nodeCounter.count, 1u);
    if (index >= uint(scene.layerParams.y)) {
        // 节点池耗尽，这一片元被丢弃
        outDummy = vec4(0.0);
        return;
    }

    nodeColors.colors[index] = inColor;
    const uint previous = atomicExchange(headBuffer.heads[pixel], index);
    nodeMeta.metas[index] = uvec2(floatBitsToUint(gl_FragCoord.z), previous);
    outDummy = vec4(0.0);
}
