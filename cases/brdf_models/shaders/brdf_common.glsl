// 微表面 BRDF、法线分布的重要性采样与方向反照率表。CPU 侧的建表代码与这里的公式一致
#ifndef BRDF_MODELS_COMMON_GLSL
#define BRDF_MODELS_COMMON_GLSL

layout(set = 0, binding = 0) uniform BrdfBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;    // xyz 相机位置, w 保留
    vec4 lightDirection;    // xyz 指向光源的单位向量, w 光照强度
    vec4 lightColor;        // rgb 光源颜色, a 均匀环境的辐射亮度
    vec4 material;          // x 粗糙度, y 金属度, z 基础颜色, w 保留
    vec4 options;           // x 光照模式, y 法线分布, z 几何项, w 多次散射补偿开关
    vec4 miscParams;        // x 菲涅耳模型, y 炉子测试采样数, z 曝光倍数, w 保留
    // 每个粗糙度上的平均方向反照率，64 个浮点打包成 16 个 vec4
    vec4 averageAlbedo[16];
} bb;

// 方向反照率表：x 是法线与视线夹角的余弦，y 是粗糙度
layout(set = 0, binding = 1) uniform sampler2D albedoTable;

const float LIGHTING_DIRECTIONAL = 0.0;
const float LIGHTING_FURNACE = 1.0;
const float LIGHTING_TABLE = 2.0;

const float DISTRIBUTION_GGX = 0.0;
const float DISTRIBUTION_BECKMANN = 1.0;
const float DISTRIBUTION_BLINN_PHONG = 2.0;

const float GEOMETRY_SMITH = 0.0;
const float GEOMETRY_SCHLICK = 1.0;
const float GEOMETRY_NONE = 2.0;

const float FRESNEL_SCHLICK = 0.0;
const float FRESNEL_CONSTANT = 1.0;

const float PI = 3.14159265359;
const int TABLE_ROUGHNESS_COUNT = 64;
const int TABLE_COSINE_COUNT = 32;

// 与色调映射 case 相同的 ACES 拟合与 sRGB 编码
vec3 encodeDisplay(vec3 color)
{
    color *= bb.miscParams.z;
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

// 法线分布：三种形状都用半角的正切或余弦的幂次表达，粗糙度先平方成 alpha
float distributionTerm(float nDotH, float roughness)
{
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float cosine2 = nDotH * nDotH;

    if (bb.options.y < DISTRIBUTION_BECKMANN - 0.5) {
        const float denominator = cosine2 * (alpha2 - 1.0) + 1.0;
        return alpha2 / (PI * denominator * denominator);
    }
    if (bb.options.y < DISTRIBUTION_BLINN_PHONG - 0.5) {
        const float tangent2 = max(1.0 - cosine2, 0.0) / max(cosine2, 1e-6);
        return exp(-tangent2 / alpha2) / max(PI * alpha2 * cosine2 * cosine2, 1e-6);
    }
    const float power = 2.0 / max(alpha2, 1e-6) - 2.0;
    return (power + 2.0) / (2.0 * PI) * pow(nDotH, power);
}

// 可见性项 V = G / (4 (n·v)(n·l))，微表面 BRDF 直接写成 D V F
float geometryVisibility(float nDotV, float nDotL, float roughness)
{
    if (bb.options.z > GEOMETRY_NONE - 0.5) {
        return 1.0 / max(4.0 * nDotV * nDotL, 1e-6);
    }
    if (bb.options.z > GEOMETRY_SCHLICK - 0.5) {
        const float k = roughness * roughness * 0.5;
        const float gv = nDotV / max(nDotV * (1.0 - k) + k, 1e-6);
        const float gl = nDotL / max(nDotL * (1.0 - k) + k, 1e-6);
        return gv * gl / max(4.0 * nDotV * nDotL, 1e-6);
    }

    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    const float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0 - alpha2) + alpha2);
    const float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0 - alpha2) + alpha2);
    return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

vec3 fresnelTerm(float cosine, vec3 f0)
{
    if (bb.miscParams.x > FRESNEL_CONSTANT - 0.5) {
        return f0;
    }
    return f0 + (1.0 - f0) * pow(max(1.0 - cosine, 0.0), 5.0);
}

// 按各个法线分布自身做重要性采样，u 是两个均匀随机数
vec3 sampleHalfVector(vec2 u, float roughness)
{
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;

    float cosine = 1.0;
    if (bb.options.y < DISTRIBUTION_BECKMANN - 0.5) {
        cosine = sqrt(max((1.0 - u.y) / (1.0 + (alpha2 - 1.0) * u.y), 0.0));
    } else if (bb.options.y < DISTRIBUTION_BLINN_PHONG - 0.5) {
        const float tangent2 = -alpha2 * log(max(1.0 - u.y, 1e-6));
        cosine = 1.0 / sqrt(1.0 + tangent2);
    } else {
        const float power = 2.0 / max(alpha2, 1e-6) - 2.0;
        cosine = pow(u.y, 1.0 / (power + 1.0));
    }

    const float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    const float phi = 2.0 * PI * u.x;
    return vec3(sine * cos(phi), sine * sin(phi), cosine);
}

float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count)
{
    return vec2(float(index) / float(count), radicalInverse(index));
}

// 方向反照率：法线方向为 n 时反射出去的能量占入射能量的比例。
// 余弦方向的两个端点是格子本身，查询时把坐标对到格子中心上；粗糙度方向本来就是格子中心
float tableDirectionalAlbedo(float cosine, float roughness)
{
    const float u = (cosine * float(TABLE_COSINE_COUNT - 1) + 0.5) / float(TABLE_COSINE_COUNT);
    return texture(albedoTable, vec2(u, roughness)).r;
}

float tableAverageAlbedo(float roughness)
{
    const int index =
        clamp(int(roughness * float(TABLE_ROUGHNESS_COUNT) - 0.5), 0, TABLE_ROUGHNESS_COUNT - 1);
    return bb.averageAlbedo[index / 4][index % 4];
}

// 单次散射的方向反照率，用重要性采样在片上积分：
// f (n·l) 除以采样概率之后只剩 4 V F (v·h)(n·l)/(n·h)
vec3 integrateSingleScattering(vec3 n, vec3 v, float roughness, vec3 f0, int sampleCount)
{
    const float nDotV = max(dot(n, v), 1e-4);
    vec3 total = vec3(0.0);
    for (int i = 0; i < sampleCount; ++i) {
        const vec3 halfVector = sampleHalfVector(hammersley(uint(i), uint(sampleCount)), roughness);
        const float vDotH = dot(v, halfVector);
        const vec3 light = reflect(-v, halfVector);
        const float nDotL = dot(n, light);
        if (vDotH <= 0.0 || nDotL <= 0.0) {
            continue;
        }
        const float nDotH = max(halfVector.z, 1e-4);
        const float visibility = geometryVisibility(nDotV, nDotL, roughness);
        total += 4.0 * visibility * fresnelTerm(vDotH, f0) * vDotH * nDotL / nDotH;
    }
    return total / float(sampleCount);
}

// 多次散射的彩色因子：白炉子测试里 F0 取 1 时它正好是 1，补偿量恰好补齐缺失的能量
vec3 multipleScatteringFactor(vec3 f0, float roughness)
{
    const float eAverage = tableAverageAlbedo(roughness);
    const vec3 fAverage = f0 + (1.0 - f0) / 21.0;
    return fAverage * fAverage * eAverage / max(1.0 - fAverage * (1.0 - eAverage), 1e-4);
}

// Kulla-Conty 的多次散射补偿项
vec3 multipleScatteringBrdf(float nDotV, float nDotL, float roughness, vec3 f0)
{
    const float eView = tableDirectionalAlbedo(nDotV, roughness);
    const float eLight = tableDirectionalAlbedo(nDotL, roughness);
    const float eAverage = tableAverageAlbedo(roughness);
    const vec3 factor = multipleScatteringFactor(f0, roughness);
    return factor * ((1.0 - eView) * (1.0 - eLight) / (PI * max(1.0 - eAverage, 1e-4)));
}

vec3 specularBrdf(vec3 n, vec3 v, vec3 l, float roughness, vec3 f0)
{
    const vec3 halfVector = normalize(v + l);
    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);
    const float nDotH = max(dot(n, halfVector), 0.0);
    const float vDotH = max(dot(v, halfVector), 0.0);

    const float d = distributionTerm(nDotH, roughness);
    const float visibility = geometryVisibility(nDotV, nDotL, roughness);
    return d * visibility * fresnelTerm(vDotH, f0);
}

#endif
