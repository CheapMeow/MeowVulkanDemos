#version 450

// 合成：场景原图加上泛光，按选定的顺序做抗锯齿与色调映射

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform BloomBuffer {
    vec4 viewportParams;
    vec4 modeParams;       // x 阈值模式, y 链实现, z 是否应用阈值, w 抗锯齿与色调映射的顺序
    vec4 miscParams;       // x 阈值, y 泛光强度, z 时间, w 是否抖动
} bloom;

layout(set = 0, binding = 1) uniform sampler2D hdrTexture;
layout(set = 0, binding = 2) uniform sampler2D bloomTexture;

vec3 tonemap(vec3 color)
{
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// 泛光权重：不做泛光时取零
float gBloomWeight;

// 沿边缘走向的一次平滑，输入既可以是高动态范围的合成值，也可以是已经色调映射过的值
vec3 antialias(vec2 uv, vec3 center)
{
    const vec2 texel = 1.0 / bloom.viewportParams.xy;
    const float lumaCenter = luminance(center);
    float lowest = lumaCenter;
    float highest = lumaCenter;
    vec3 sum = center;
    for (int i = 0; i < 4; ++i) {
        const vec2 offset = (i == 0)   ? vec2(texel.x, 0.0)
                            : (i == 1) ? vec2(-texel.x, 0.0)
                            : (i == 2) ? vec2(0.0, texel.y)
                                       : vec2(0.0, -texel.y);
        const vec3 neighbor = texture(hdrTexture, uv + offset).rgb +
                              texture(bloomTexture, uv + offset).rgb * gBloomWeight;
        const float sampleLuma = luminance(neighbor);
        lowest = min(lowest, sampleLuma);
        highest = max(highest, sampleLuma);
        sum += neighbor;
    }
    const float contrast = highest - lowest;
    const float blend = clamp(contrast * 1.5, 0.0, 1.0);
    return mix(center, sum / 5.0, blend);
}

void main()
{
    const vec3 scene = texture(hdrTexture, inUv).rgb;
    // 不做泛光时低分辨率层没有被写过，权重取零把它排除掉
    gBloomWeight = bloom.modeParams.x < 0.5 ? 0.0 : bloom.miscParams.y;
    const vec3 bloomed = scene + texture(bloomTexture, inUv).rgb * gBloomWeight;

    vec3 result;
    if (bloom.modeParams.w < 0.5) {
        // 抗锯齿排在色调映射之前：在高的动态范围上做平滑，再压缩
        result = tonemap(antialias(inUv, bloomed));
    } else {
        // 抗锯齿排在色调映射之后：先压缩到显示范围，再平滑，高光已经被压扁
        const vec3 mapped = tonemap(bloomed);
        const vec3 smoothed = antialias(inUv, bloomed);
        result = mix(mapped, tonemap(smoothed), 0.5);
    }
    outColor = vec4(result, 1.0);
}
