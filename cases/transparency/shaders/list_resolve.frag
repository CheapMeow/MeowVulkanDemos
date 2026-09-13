#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;
    vec4 colorParams;
    vec4 modeParams;
    vec4 layerParams;
} scene;

layout(set = 0, binding = 4) buffer HeadBuffer {
    uint heads[];
} headBuffer;

layout(set = 0, binding = 5) buffer NodeColorBuffer {
    vec4 colors[];
} nodeColors;

layout(set = 0, binding = 6) buffer NodeMetaBuffer {
    uvec2 metas[];
} nodeMeta;

layout(set = 0, binding = 7) buffer LayerCountBuffer {
    uint counts[];
} layerCount;

const int MAX_LIST = 16;

vec3 heatColor(uint count)
{
    float t = clamp(float(count) / float(scene.layerParams.x), 0.0, 1.0);
    vec3 low = vec3(0.05, 0.08, 0.30);
    vec3 mid = vec3(0.10, 0.75, 0.35);
    vec3 high = vec3(1.00, 0.85, 0.20);
    return t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, t * 2.0 - 1.0);
}

// 逐像素链表的结果：收集本像素链表上的片元，按深度从远到近排序后依次混合
void main()
{
    const uint pixel = uint(gl_FragCoord.y) * uint(scene.viewportParams.x) + uint(gl_FragCoord.x);

    vec4 colors[MAX_LIST];
    float depths[MAX_LIST];
    int found = 0;
    uint index = headBuffer.heads[pixel];
    while (index != 0xFFFFFFFFu && found < MAX_LIST) {
        colors[found] = nodeColors.colors[index];
        depths[found] = uintBitsToFloat(nodeMeta.metas[index].x);
        index = nodeMeta.metas[index].y;
        ++found;
    }

    // 插入排序，深度大的排在前面，也就是由远到近
    for (int i = 1; i < found; ++i) {
        vec4 color = colors[i];
        float depth = depths[i];
        int j = i - 1;
        while (j >= 0 && depths[j] < depth) {
            colors[j + 1] = colors[j];
            depths[j + 1] = depths[j];
            --j;
        }
        colors[j + 1] = color;
        depths[j + 1] = depth;
    }

    vec3 result = scene.colorParams.rgb;
    for (int i = 0; i < found; ++i) {
        result = mix(result, colors[i].rgb, colors[i].a);
    }

    if (scene.modeParams.y > 0.5) {
        result = heatColor(layerCount.counts[pixel]);
    }
    outColor = vec4(result, 1.0);
}
