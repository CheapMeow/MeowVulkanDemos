#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(push_constant) uniform FxaaParams {
    vec4 params;  // xy 一个纹素的尺寸, z 边缘阈值, w 搜索步数
} fxaa;

float luma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

float lumaAt(vec2 uv)
{
    return luma(texture(sceneColor, uv).rgb);
}

// 按亮度差找边缘，再沿边缘走向做一次带权平滑。它与硬件多重采样的区别在于：
// 输入只有已经光栅化完、次像素信息已经被丢掉的最终图像
void main()
{
    vec2 texel = fxaa.params.xy;
    float edgeThreshold = fxaa.params.z;
    int searchSteps = int(fxaa.params.w);

    vec3 center = texture(sceneColor, inUv).rgb;
    float lumaM = luma(center);
    float lumaN = lumaAt(inUv + vec2(0.0, -texel.y));
    float lumaS = lumaAt(inUv + vec2(0.0, texel.y));
    float lumaE = lumaAt(inUv + vec2(texel.x, 0.0));
    float lumaW = lumaAt(inUv + vec2(-texel.x, 0.0));
    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float contrast = lumaMax - lumaMin;

    // 亮度变化小于阈值的位置不当作边缘，原样输出
    if (contrast < max(0.0312, lumaMax * edgeThreshold)) {
        outColor = vec4(center, 1.0);
        return;
    }

    // 梯度指向横穿边缘的方向，沿边缘的走向与它垂直
    vec2 tangent = normalize(vec2(-(lumaS - lumaN), lumaE - lumaW) + vec2(1e-5));
    vec2 stepVec = tangent * texel;

    // 沿边缘两侧搜索，亮度与中心接近的采样才会被计入，越远权重越低
    vec3 accumulated = center;
    float totalWeight = 1.0;
    for (int i = 1; i <= searchSteps; ++i) {
        float distance = float(i);
        float falloff = 1.0 / distance;
        vec3 sampleA = texture(sceneColor, inUv + stepVec * distance).rgb;
        vec3 sampleB = texture(sceneColor, inUv - stepVec * distance).rgb;
        float weightA = falloff * max(0.0, 1.0 - abs(luma(sampleA) - lumaM) / max(contrast, 1e-4));
        float weightB = falloff * max(0.0, 1.0 - abs(luma(sampleB) - lumaM) / max(contrast, 1e-4));
        accumulated += sampleA * weightA + sampleB * weightB;
        totalWeight += weightA + weightB;
    }

    vec3 filtered = accumulated / totalWeight;
    // 对比度越高混合越多，低对比处保持原样
    float blend = clamp(contrast * 2.0, 0.0, 1.0);
    outColor = vec4(mix(center, filtered, blend), 1.0);
}
