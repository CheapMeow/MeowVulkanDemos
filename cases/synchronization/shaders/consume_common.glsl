// 两个消费者（采样图案与按输入附件读图案）共用的着色。图案纹理是整数格式，读到的是
// 精确的字节值，两条读取路径给出相同的数值。
// 消费结果依赖图案的具体数值，读到的图案如果落后一帧，画面立刻不同
vec3 shadePattern(uvec3 texel, vec2 uv)
{
    precise vec3 pattern = vec3(texel) / 255.0;

    precise vec2 centered = uv * 2.0 - 1.0;
    precise float vignette = smoothstep(1.15, 0.2, length(centered));
    precise vec3 tinted = pattern * (0.4 + 0.6 * vignette);
    return pow(tinted, vec3(1.0 / 1.15));
}
