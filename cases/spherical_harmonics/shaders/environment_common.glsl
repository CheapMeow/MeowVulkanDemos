// 解析环境函数、实球谐基与漫反射卷积。CPU 侧有一份常数完全一致的实现
#ifndef SPHERICAL_HARMONICS_COMMON_GLSL
#define SPHERICAL_HARMONICS_COMMON_GLSL

layout(set = 0, binding = 0) uniform ShBuffer {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;    // xyz 相机位置, w 保留
    vec4 options;           // x 阶数, y 背景模式, z 着色模式, w 参考积分的采样数
    vec4 rotationParams;    // x 环境绕 Y 轴的旋转角, yzw 保留
    vec4 skyParams;         // x 天空亮度, y 光晕亮度, z 光晕锐度（余弦幂次）, w 保留
    vec4 sunDirection;      // xyz 太阳方向, w 保留
    vec4 displayParams;     // x 曝光倍数, yzw 保留
    vec4 sh[21];            // 红通道 7 个 vec4 装 25 个系数，其后依次是绿与蓝
} sb;

const int SH_MAX_COEFFICIENTS = 25;

const float BACKGROUND_SH = 0.0;
const float BACKGROUND_ORIGINAL = 1.0;

const float SHADING_SH = 0.0;
const float SHADING_REFERENCE = 1.0;

// 与色调映射 case 相同的 ACES 拟合与 sRGB 编码，让高动态范围的环境能显示出来
vec3 encodeDisplay(vec3 color)
{
    color *= sb.displayParams.x;
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    const vec3 mapped = clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
    const vec3 low = mapped * 12.92;
    const vec3 high = 1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), mapped));
}

const float SH_CONVOLUTION[5] = float[5](3.14159265, 2.09439510, 0.78539816, 0.0, -0.13089969);
const int SH_BAND_OF[25] = int[25](0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
                                   4, 4, 4, 4, 4, 4, 4, 4, 4);

// 绕 Y 轴旋转方向，用于把采样方向转到未旋转的环境坐标系里
vec3 rotateAboutY(vec3 d, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return vec3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}

// 实球谐基：索引按 l 从 0 到 4、每个 l 内 m 从 -l 到 l 排列。
// 常用表格把 z 当作极轴，这里把 y 与 z 对调，让极轴与场景的上方向一致，
// 于是绕 Y 轴旋转就只混合同一阶里的 ±m 两个分量
float shBasis(int index, vec3 d)
{
    const float x = d.x;
    const float y = d.y;
    const float z = d.z;

    if (index == 0) { return 0.2820947918; }
    if (index == 1) { return 0.4886025119 * z; }
    if (index == 2) { return 0.4886025119 * y; }
    if (index == 3) { return 0.4886025119 * x; }
    if (index == 4) { return 1.0925484306 * x * z; }
    if (index == 5) { return 1.0925484306 * z * y; }
    if (index == 6) { return 0.3153915653 * (3.0 * y * y - 1.0); }
    if (index == 7) { return 1.0925484306 * x * y; }
    if (index == 8) { return 0.5462742153 * (x * x - z * z); }
    if (index == 9) { return 0.5900435899 * z * (3.0 * x * x - z * z); }
    if (index == 10) { return 2.8906114426 * x * z * y; }
    if (index == 11) { return 0.4570457995 * z * (5.0 * y * y - 1.0); }
    if (index == 12) { return 0.3731763326 * y * (5.0 * y * y - 3.0); }
    if (index == 13) { return 0.4570457995 * x * (5.0 * y * y - 1.0); }
    if (index == 14) { return 1.4453057213 * y * (x * x - z * z); }
    if (index == 15) { return 0.5900435899 * x * (x * x - 3.0 * z * z); }
    if (index == 16) { return 2.5033429418 * x * z * (x * x - z * z); }
    if (index == 17) { return 1.7701307698 * z * y * (3.0 * x * x - z * z); }
    if (index == 18) { return 0.9461746958 * x * z * (7.0 * y * y - 1.0); }
    if (index == 19) { return 0.6690465436 * z * y * (7.0 * y * y - 3.0); }
    if (index == 20) { return 0.1057855469 * (35.0 * y * y * y * y - 30.0 * y * y + 3.0); }
    if (index == 21) { return 0.6690465436 * x * y * (7.0 * y * y - 3.0); }
    if (index == 22) { return 0.4730873479 * (7.0 * y * y - 1.0) * (x * x - z * z); }
    if (index == 23) { return 1.7701307698 * x * y * (x * x - 3.0 * z * z); }
    return 0.6258357354 * (x * x * x * x - 6.0 * x * x * z * z + z * z * z * z);
}

// 系数在 uniform 里是三个通道各七个 vec4，逐个取出单个分量
float shCoefficient(int channel, int index)
{
    return sb.sh[channel * 7 + index / 4][index % 4];
}

vec3 shCoefficientRgb(int index)
{
    return vec3(shCoefficient(0, index), shCoefficient(1, index), shCoefficient(2, index));
}

// 环境重建：系数乘基函数再求和
vec3 shRadiance(vec3 d, int bandCount)
{
    vec3 result = vec3(0.0);
    for (int i = 0; i < bandCount * bandCount; ++i) {
        result += shCoefficientRgb(i) * shBasis(i, d);
    }
    return result;
}

// 漫反射辐照度：每一项再乘上卷积因子，三阶的因子为零，因此三阶系数对辐照度没有贡献
vec3 shIrradiance(vec3 n, int bandCount)
{
    vec3 result = vec3(0.0);
    for (int i = 0; i < bandCount * bandCount; ++i) {
        result += shCoefficientRgb(i) * shBasis(i, n) * SH_CONVOLUTION[SH_BAND_OF[i]];
    }
    return result;
}

// 解析环境函数：天顶到地平线的渐变、经过一段平滑过渡落到暗地面，再加上一个余弦幂次的光晕
vec3 environmentRadiance(vec3 direction)
{
    const vec3 d = normalize(direction);

    const vec3 zenith = vec3(0.02, 0.04, 0.09);
    const vec3 horizon = vec3(0.35, 0.42, 0.58);
    const vec3 ground = vec3(0.045, 0.045, 0.05);

    const float up = max(d.y, 0.0);
    const vec3 sky = mix(horizon, zenith, pow(up, 1.6));
    const vec3 base = mix(ground, sky, smoothstep(-0.3, 0.3, d.y)) * sb.skyParams.x;

    const float cosAngle = max(dot(d, normalize(sb.sunDirection.xyz)), 0.0);
    const vec3 glow = vec3(1.0, 0.92, 0.78) * sb.skyParams.y * pow(cosAngle, sb.skyParams.z);

    return base + glow;
}

// 逐像素的参考积分：在余弦加权的半球上按分层采样求平均，估计量是 π 乘平均值
vec3 integratedIrradiance(vec3 n, int sampleCount)
{
    const vec3 normal = normalize(n);
    const vec3 helper = abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent = normalize(cross(helper, normal));
    const vec3 bitangent = cross(normal, tangent);

    const int side = int(sqrt(float(sampleCount)));
    const float step = 1.0 / float(side);

    vec3 accum = vec3(0.0);
    for (int i = 0; i < side; ++i) {
        for (int j = 0; j < side; ++j) {
            const float u = (float(i) + 0.5) * step;
            const float v = (float(j) + 0.5) * step;
            const float r = sqrt(u);
            const float phi = 6.2831853 * v;
            const vec3 local = vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u, 0.0)));
            const vec3 direction = tangent * local.x + bitangent * local.y + normal * local.z;
            accum += environmentRadiance(rotateAboutY(direction, -sb.rotationParams.x));
        }
    }

    return accum / float(side * side) * 3.14159265;
}

#endif
