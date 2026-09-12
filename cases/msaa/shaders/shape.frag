#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in float inUseAlphaToCoverage;
layout(location = 2) flat in vec3 inColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;  // xy 视口尺寸, zw 保留
    vec4 colorParams;     // rgb 背景亮度, a 保留
    vec4 modeParams;      // x 子场景编号, y 是否用 alpha to coverage, z 片元开销, w 保留
} scene;

// 片元调用计数：每执行一次片元着色器加一，用来核对多重采样不放大着色次数
layout(set = 0, binding = 2) buffer CounterBuffer {
    uint fragmentCount;
    uint padding[3];
} counter;

layout(location = 0) out vec4 outColor;

// 程序生成的镂空掩码，用于 alpha test 与 alpha to coverage
float coverageMask(vec2 uv)
{
    float a = sin(uv.x * 37.0) * sin(uv.y * 41.0);
    float b = sin(uv.x * 13.0 + 1.7) * sin(uv.y * 17.0 + 0.9);
    return step(0.0, a + 0.6 * b);
}

// 可调长度的运算，用来放大着色开销
float expensiveValue(vec2 uv, float iterations)
{
    float value = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (float(i) >= iterations) {
            break;
        }
        float t = float(i) * 0.017 + uv.x * 3.1;
        value += sin(t) * cos(t * 1.3 + uv.y * 2.7) * 0.5 + 0.5;
    }
    return fract(value * 0.01);
}

void main()
{
    atomicAdd(counter.fragmentCount, 1u);

    vec3 color = inColor * (0.85 + 0.3 * expensiveValue(inUv, scene.modeParams.z));

    // 镂空的形状走 alpha test 或 alpha to coverage
    if (inUseAlphaToCoverage > 0.5) {
        float mask = coverageMask(inUv);
        if (scene.modeParams.y < 0.5) {
            if (mask < 0.5) {
                discard;
            }
        }
        outColor = vec4(color, mask);
        return;
    }

    outColor = vec4(color, 1.0);
}
