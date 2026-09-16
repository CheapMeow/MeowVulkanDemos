#version 450

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform NoiseBuffer {
    vec4 params;   // x 噪声种类, y 频率, z 倍频数, w 持续度
    vec4 misc;     // x 间隙度, y 时间, z 参考超采样数（每边的平方根）, w 显示缩放
} nb;

const float KIND_VALUE = 0.0;
const float KIND_PERLIN = 1.0;
const float KIND_WORLEY = 2.0;
const float KIND_FBM = 3.0;
const float KIND_REFERENCE = 4.0;

// 把二维整数格点映射成一个伪随机数：正弦散列，整数格点上是确定值
float hash(vec2 cell)
{
    return fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453123);
}

vec2 hashGradient(vec2 cell)
{
    const float angle = hash(cell) * 6.28318530718;
    return vec2(cos(angle), sin(angle));
}

// Perlin 的五次曲线：一阶与二阶导数在两端都为零，格点之间不会出现折线
float fade(float t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// 值噪声：格点上是随机数，格内按五次曲线插值
float valueNoise(vec2 point)
{
    const vec2 base = floor(point);
    const vec2 f = point - base;
    const vec2 u = vec2(fade(f.x), fade(f.y));

    const float bottomLeft = hash(base);
    const float bottomRight = hash(base + vec2(1.0, 0.0));
    const float topLeft = hash(base + vec2(0.0, 1.0));
    const float topRight = hash(base + vec2(1.0, 1.0));

    return mix(mix(bottomLeft, bottomRight, u.x), mix(topLeft, topRight, u.x), u.y);
}

// 梯度噪声：格点上是随机方向，四个角的贡献是梯度与相对位置的点积
float perlinNoise(vec2 point)
{
    const vec2 base = floor(point);
    const vec2 f = point - base;
    const vec2 u = vec2(fade(f.x), fade(f.y));

    const float bottomLeft = dot(hashGradient(base), f);
    const float bottomRight = dot(hashGradient(base + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    const float topLeft = dot(hashGradient(base + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    const float topRight = dot(hashGradient(base + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));

    const float value = mix(mix(bottomLeft, bottomRight, u.x), mix(topLeft, topRight, u.x), u.y);
    // 二维梯度噪声的取值大致落在负零点七到零点七之间
    return clamp(value * 0.7071 + 0.5, 0.0, 1.0);
}

// 细胞噪声：每个格子里放一个特征点，取到最近特征点的距离
float worleyNoise(vec2 point)
{
    const vec2 base = floor(point);
    float shortest = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 cell = base + vec2(float(x), float(y));
            const vec2 feature = cell + vec2(hash(cell), hash(cell + vec2(17.0, 31.0)));
            shortest = min(shortest, length(feature - point));
        }
    }
    return clamp(shortest, 0.0, 1.0);
}

float baseNoise(vec2 point, float kind)
{
    if (kind < KIND_PERLIN - 0.5) {
        return valueNoise(point);
    }
    if (kind < KIND_WORLEY - 0.5) {
        return perlinNoise(point);
    }
    return worleyNoise(point);
}

// 多倍频叠加：每一层频率乘间隙度、振幅乘持续度，最后按振幅之和归一化
float layeredNoise(vec2 point, float kind, int octaves, float persistence, float lacunarity)
{
    float total = 0.0;
    float amplitude = 1.0;
    float weight = 0.0;
    float scale = 1.0;

    for (int octave = 0; octave < octaves; ++octave) {
        total += baseNoise(point * scale, kind) * amplitude;
        weight += amplitude;
        amplitude *= persistence;
        scale *= lacunarity;
    }
    return weight > 0.0 ? total / weight : 0.0;
}

float evaluate(vec2 point)
{
    const float kind = nb.params.x;
    const float frequency = nb.params.y;
    const int octaves = int(nb.params.z);
    const float persistence = nb.params.w;
    const float lacunarity = nb.misc.x;

    if (kind > KIND_REFERENCE - 0.5) {
        // 参考：对像素覆盖的区域做超采样，等价于把高频压掉之后的正确结果。
        // 像素在噪声空间里的足迹由屏幕空间导数给出
        const int side = int(nb.misc.z);
        const vec2 footprint = fwidth(point * frequency);
        float total = 0.0;
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const vec2 offset = (vec2(float(x), float(y)) + 0.5) / float(side) - 0.5;
                total += layeredNoise(point * frequency + offset * footprint, KIND_PERLIN,
                                      octaves, persistence, lacunarity);
            }
        }
        return total / float(side * side);
    }

    if (kind > KIND_FBM - 0.5) {
        return layeredNoise(point * frequency, KIND_PERLIN, octaves, persistence, lacunarity);
    }
    return baseNoise(point * frequency, kind);
}

void main()
{
    // 时间项让图案缓慢平移，便于观察时域上的闪烁
    const vec2 uv = inUv + vec2(nb.misc.y * 0.05, 0.0);
    const float value = evaluate(uv);
    outColor = vec4(vec3(clamp(value * nb.misc.w, 0.0, 1.0)), 1.0);
}
