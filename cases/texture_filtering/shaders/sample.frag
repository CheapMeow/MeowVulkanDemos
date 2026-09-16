#version 450

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SampleBuffer {
    vec4 params;   // x 过滤方式, y 缩放, z 纹理尺寸, w 参考的超采样数（每边的平方根）
    vec4 misc;     // x 显示缩放, yzw 保留
} sb;

layout(set = 0, binding = 1) uniform sampler2D patternTexture;

const float MODE_NEAREST = 0.0;
const float MODE_BILINEAR = 1.0;
const float MODE_BSPLINE = 2.0;
const float MODE_CATMULL_ROM = 3.0;
const float MODE_LANCZOS2 = 4.0;
const float MODE_LANCZOS3 = 5.0;
const float MODE_ANALYTIC = 6.0;

const float PI = 3.14159265359;

// 与 CPU 侧同名函数保持一致。参考路径直接在解析图案上取值，不经过纹理
vec3 patternColor(vec2 uv)
{
    const vec2 first = uv - vec2(0.38, 0.42);
    const vec2 second = uv - vec2(0.66, 0.60);
    const float firstPhase = dot(first, first) * 4.0 * 70.0;
    const float secondPhase = dot(second, second) * 4.0 * 70.0;

    return vec3(0.5 + 0.5 * cos(firstPhase),
                0.5 + 0.5 * cos(firstPhase * 1.05 + 1.0),
                0.5 + 0.5 * cos(secondPhase * 1.10 + 2.0));
}

vec3 fetchTexel(ivec2 texel)
{
    // 重复寻址：缩小时纹理在画面里平铺多份
    const int size = int(sb.params.z);
    const ivec2 wrapped = texel - ivec2(size) * ivec2(floor(vec2(texel) / float(size)));
    return texelFetch(patternTexture, wrapped, 0).rgb;
}

// 纹理坐标换算到纹素坐标：整数部分是纹素下标，小数部分是纹素内部的位置
vec2 texelPosition(vec2 uv)
{
    return uv * sb.params.z - 0.5;
}

vec3 sampleNearest(vec2 uv)
{
    const vec2 position = texelPosition(uv);
    return fetchTexel(ivec2(floor(position + 0.5)));
}

vec3 sampleBilinear(vec2 uv)
{
    const vec2 position = texelPosition(uv);
    const vec2 base = floor(position);
    const vec2 f = position - base;

    const vec3 topLeft = fetchTexel(ivec2(base));
    const vec3 topRight = fetchTexel(ivec2(base) + ivec2(1, 0));
    const vec3 bottomLeft = fetchTexel(ivec2(base) + ivec2(0, 1));
    const vec3 bottomRight = fetchTexel(ivec2(base) + ivec2(1, 1));

    return mix(mix(topLeft, topRight, f.x), mix(bottomLeft, bottomRight, f.x), f.y);
}

// 四种可分离的立方核权重：三阶 B 样条、Catmull-Rom、Lanczos2、Lanczos3。
// 参数 x 是采样点到纹素中心的距离
float kernelWeight(float x, float mode)
{
    if (mode < MODE_CATMULL_ROM - 0.5) {
        // 三阶 B 样条，权重全为正，结果不会过冲
        if (x < -2.0 || x > 2.0) {
            return 0.0;
        }
        const float a = abs(x);
        if (a < 1.0) {
            return (2.0 / 3.0) - a * a + 0.5 * a * a * a;
        }
        const float t = 2.0 - a;
        return t * t * t / 6.0;
    }
    if (mode < MODE_LANCZOS2 - 0.5) {
        // Catmull-Rom，等价于 B 样条加一层锐化
        const float a = abs(x);
        if (a < 1.0) {
            return 1.5 * a * a * a - 2.5 * a * a + 1.0;
        }
        if (a < 2.0) {
            return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
        }
        return 0.0;
    }

    const float radius = mode < MODE_LANCZOS3 - 0.5 ? 2.0 : 3.0;
    if (abs(x) >= radius) {
        return 0.0;
    }
    // 截断的 sinc：主瓣之外还带负旁瓣，因此边缘上会出现过冲。
    // 两个因子在零点都要单独处理，否则会出现零除
    const float px = PI * x;
    const float pxr = px / radius;
    const float sinc = abs(x) < 1e-5 ? 1.0 : sin(px) / px;
    const float sincR = abs(pxr) < 1e-5 ? 1.0 : sin(pxr) / pxr;
    return sinc * sincR;
}

vec3 sampleKernel(vec2 uv, float mode)
{
    const int radius = mode < MODE_LANCZOS3 - 0.5 ? 2 : 3;
    const vec2 position = texelPosition(uv);
    const vec2 base = floor(position);
    const vec2 f = position - base;

    vec3 accum = vec3(0.0);
    float weightSum = 0.0;
    for (int j = -radius + 1; j <= radius; ++j) {
        for (int i = -radius + 1; i <= radius; ++i) {
            const float weight =
                kernelWeight(float(i) - f.x, mode) * kernelWeight(float(j) - f.y, mode);
            accum += fetchTexel(ivec2(base) + ivec2(i, j)) * weight;
            weightSum += weight;
        }
    }
    return accum / max(weightSum, 1e-5);
}

// 参考：按像素覆盖的区域对解析图案做超采样，等价于理想的重建。
// 采样位置取小数部分，与纹理的重复寻址对齐，超采样跨越图案边界时也被平均进去
vec3 sampleAnalytic(vec2 uv, vec2 footprint)
{
    const int side = int(sb.params.w);
    if (side <= 1) {
        return patternColor(fract(uv));
    }
    vec3 accum = vec3(0.0);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const vec2 offset = (vec2(float(x), float(y)) + 0.5) / float(side) - 0.5;
            accum += patternColor(fract(uv + offset * footprint));
        }
    }
    return accum / float(side * side);
}

void main()
{
    // 缩放小于 1 是放大纹理，大于 1 是缩小
    const vec2 uv = (inUv - 0.5) * sb.params.y + 0.5;
    const vec2 footprint = vec2(sb.params.y) / vec2(textureSize(patternTexture, 0));

    vec3 color;
    if (sb.params.x > MODE_ANALYTIC - 0.5) {
        color = sampleAnalytic(uv, footprint);
    } else if (sb.params.x > MODE_LANCZOS3 - 0.5) {
        color = sampleKernel(uv, MODE_LANCZOS3);
    } else if (sb.params.x > MODE_LANCZOS2 - 0.5) {
        color = sampleKernel(uv, MODE_LANCZOS2);
    } else if (sb.params.x > MODE_CATMULL_ROM - 0.5) {
        color = sampleKernel(uv, MODE_CATMULL_ROM);
    } else if (sb.params.x > MODE_BSPLINE - 0.5) {
        color = sampleKernel(uv, MODE_BSPLINE);
    } else if (sb.params.x > MODE_BILINEAR - 0.5) {
        color = sampleBilinear(uv);
    } else {
        color = sampleNearest(uv);
    }

    outColor = vec4(clamp(color * sb.misc.x, 0.0, 1.0), 1.0);
}
