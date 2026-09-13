#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform ThroughputBuffer {
    vec4 viewportParams;
    vec4 kernelParams;
    vec4 miscParams;
} throughput;

layout(set = 1, binding = 0) buffer ResultBuffer {
    float values[];
} result;

// 把计算内核的结果按灰度显示出来，保证计算不会被优化掉
void main()
{
    const ivec2 size = ivec2(throughput.viewportParams.xy);
    const ivec2 pixel = ivec2(inUv * vec2(size));
    const float value = result.values[pixel.y * size.x + pixel.x];
    outColor = vec4(vec3(clamp(value, 0.0, 1.0)), 1.0);
}
