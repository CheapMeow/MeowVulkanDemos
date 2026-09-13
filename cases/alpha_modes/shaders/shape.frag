#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in float inAlpha;
layout(location = 2) flat in vec3 inColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 colorParams;      // rgb 背景亮度, a 保留
    vec4 modeParams;       // x 处理方式, y alpha 阈值, zw 保留
    vec4 animationParams;  // x 累计旋转角度, yzw 保留
} scene;

layout(set = 0, binding = 2) buffer FragmentCounter {
    uint count;
} counter;

layout(location = 0) out vec4 outColor;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// 程序生成的草丛掩码：一批向上收窄并弯曲的叶片叠加。每片叶片中间是实心，外侧留一段
// 固定宽度的渐变，这段渐变正是 alpha test 的硬边与 alpha to coverage 的过渡之间的差别所在
float vegetationMask(vec2 uv)
{
    const float EDGE = 0.007;
    float alpha = 0.0;
    for (int i = 0; i < 26; ++i) {
        float fi = float(i);
        float base = hash11(fi * 7.13 + 1.0);
        float height = 0.55 + 0.45 * hash11(fi * 3.71 + 2.0);
        float halfWidth = 0.016 + 0.018 * hash11(fi * 5.19 + 3.0);
        float lean = (hash11(fi * 2.37 + 4.0) - 0.5) * 0.45;
        float t = clamp(uv.y / height, 0.0, 1.0);
        float centerX = base + lean * t * t;
        float w = halfWidth * (1.0 - t) + EDGE;
        float dx = abs(uv.x - centerX);
        float blade = smoothstep(w, w - EDGE, dx) * step(t, 0.999);
        alpha = max(alpha, blade);
    }
    return clamp(alpha, 0.0, 1.0);
}

void main()
{
    // 片元调用次数在这里累加，帧末经缓冲回读。它反映的是真正进入着色器的片元数，
    // 被深度测试提前丢掉的片元不会计数
    atomicAdd(counter.count, 1u);

    float alpha = inAlpha * vegetationMask(inUv);

    float mode = scene.modeParams.x;
    if (mode < 0.5) {
        // alpha test：低于阈值的片元直接丢弃，边界是硬的
        if (alpha < scene.modeParams.y) {
            discard;
        }
        alpha = 1.0;
    }
    // alpha blend 保留 alpha 交给固定功能混合；alpha to coverage 也保留 alpha 交给管线状态

    outColor = vec4(inColor, alpha);
}
