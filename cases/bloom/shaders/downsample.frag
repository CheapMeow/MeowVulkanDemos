#version 450

// 第一趟顺便把亮部取出来，之后的每一趟都是对上一张降采样。
// 三种实现只在取样核上有区别

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform BloomBuffer {
    vec4 viewportParams;
    vec4 modeParams;       // x 阈值模式, y 链实现, z 是否应用阈值, w 层数
    vec4 miscParams;       // x 阈值, y 泛光强度, z 时间, w 是否抖动
} bloom;

layout(set = 0, binding = 1) uniform sampler2D sourceTexture;

const float CHAIN_GAUSSIAN = 0.0;
const float CHAIN_KAWASE = 1.0;

vec3 sampleAt(vec2 uv)
{
    return texture(sourceTexture, uv).rgb;
}

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(sourceTexture, 0));
    vec3 color;

    if (bloom.modeParams.y < CHAIN_KAWASE + 0.5) {
        // 逐级高斯：四角与四边各取一个点，权重按距离给出
        const float weights[5] = { 0.227027, 0.194594, 0.121621, 0.054054, 0.016216 };
        color = sampleAt(inUv) * weights[0];
        for (int i = 1; i < 5; ++i) {
            const vec2 offset = texel * float(i) * 1.25;
            color += sampleAt(inUv + vec2(offset.x, 0.0)) * weights[i];
            color += sampleAt(inUv - vec2(offset.x, 0.0)) * weights[i];
            color += sampleAt(inUv + vec2(0.0, offset.y)) * weights[i];
            color += sampleAt(inUv - vec2(0.0, offset.y)) * weights[i];
        }
        color /= 4.0;
    } else {
        // 一降一升的 Kawase 方式：四角加中心，核更大而取样更少
        color = sampleAt(inUv) * 4.0;
        color += sampleAt(inUv + vec2(-texel.x, -texel.y));
        color += sampleAt(inUv + vec2(texel.x, -texel.y));
        color += sampleAt(inUv + vec2(-texel.x, texel.y));
        color += sampleAt(inUv + vec2(texel.x, texel.y));
        color /= 8.0;
    }

    if (bloom.modeParams.z > 0.5) {
        // 亮部取出：硬阈值直接截断，软阈值用一段过渡压过去
        const float threshold = bloom.miscParams.x;
        float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
        if (bloom.modeParams.x < 0.5) {
            color *= step(threshold, luminance);
        } else {
            const float knee = threshold * 0.6;
            const float factor = clamp((luminance - threshold + knee) / max(knee, 1e-4), 0.0, 1.0);
            color *= factor * factor;
        }
    }

    outColor = vec4(color, 1.0);
}
