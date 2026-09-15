#include "scene_setup.h"

#include "timing.h"

#include <algorithm>
#include <cmath>

static const double PI = 3.14159265358979323846;

EnvironmentParams defaultEnvironmentParams()
{
    EnvironmentParams params = {};
    params.sunDirection = glm::normalize(glm::vec3(0.35f, 0.45f, -0.82f));
    params.skyIntensity = 1.0f;
    params.glowIntensity = 4.0f;
    params.glowExponent = 4.0f;
    return params;
}

glm::vec3 environmentRadiance(const glm::vec3& direction, const EnvironmentParams& params)
{
    const glm::vec3 d = glm::normalize(direction);

    static const glm::vec3 zenith(0.02f, 0.04f, 0.09f);
    static const glm::vec3 horizon(0.35f, 0.42f, 0.58f);
    static const glm::vec3 ground(0.045f, 0.045f, 0.05f);

    // 地平线附近用一段平滑过渡把天空与地面接起来，硬边会让球谐展开收敛得很慢
    const float up = std::max(d.y, 0.0f);
    const glm::vec3 sky = glm::mix(horizon, zenith, std::pow(up, 1.6f));
    const float blend = glm::smoothstep(-0.3f, 0.3f, d.y);
    const glm::vec3 base = glm::mix(ground, sky, blend) * params.skyIntensity;

    // 光晕是余弦的幂次，指数越大越集中，一阶指数是一个覆盖整个上半球的宽光斑
    const float cosAngle = std::max(glm::dot(d, glm::normalize(params.sunDirection)), 0.0f);
    const float lobe = std::pow(cosAngle, params.glowExponent);
    const glm::vec3 glow = glm::vec3(1.0f, 0.92f, 0.78f) * params.glowIntensity * lobe;

    return base + glow;
}

// 实球谐基，索引按 l 从 0 到 4、每个 l 内 m 从 -l 到 l 排列。
// 常用表格把 z 当作极轴，这里把 y 与 z 对调，让极轴与场景的上方向一致，
// 于是绕 Y 轴旋转就只混合同一阶里的 ±m 两个分量
static void evaluateShBasis(const glm::dvec3& d, double* outBasis)
{
    const double x = d.x;
    const double y = d.y;
    const double z = d.z;

    outBasis[0] = 0.2820947918;
    outBasis[1] = 0.4886025119 * z;
    outBasis[2] = 0.4886025119 * y;
    outBasis[3] = 0.4886025119 * x;
    outBasis[4] = 1.0925484306 * x * z;
    outBasis[5] = 1.0925484306 * z * y;
    outBasis[6] = 0.3153915653 * (3.0 * y * y - 1.0);
    outBasis[7] = 1.0925484306 * x * y;
    outBasis[8] = 0.5462742153 * (x * x - z * z);
    outBasis[9] = 0.5900435899 * z * (3.0 * x * x - z * z);
    outBasis[10] = 2.8906114426 * x * z * y;
    outBasis[11] = 0.4570457995 * z * (5.0 * y * y - 1.0);
    outBasis[12] = 0.3731763326 * y * (5.0 * y * y - 3.0);
    outBasis[13] = 0.4570457995 * x * (5.0 * y * y - 1.0);
    outBasis[14] = 1.4453057213 * y * (x * x - z * z);
    outBasis[15] = 0.5900435899 * x * (x * x - 3.0 * z * z);
    outBasis[16] = 2.5033429418 * x * z * (x * x - z * z);
    outBasis[17] = 1.7701307698 * z * y * (3.0 * x * x - z * z);
    outBasis[18] = 0.9461746958 * x * z * (7.0 * y * y - 1.0);
    outBasis[19] = 0.6690465436 * z * y * (7.0 * y * y - 3.0);
    outBasis[20] = 0.1057855469 * (35.0 * y * y * y * y - 30.0 * y * y + 3.0);
    outBasis[21] = 0.6690465436 * x * y * (7.0 * y * y - 3.0);
    outBasis[22] = 0.4730873479 * (7.0 * y * y - 1.0) * (x * x - z * z);
    outBasis[23] = 1.7701307698 * x * y * (x * x - 3.0 * z * z);
    outBasis[24] = 0.6258357354 * (x * x * x * x - 6.0 * x * x * z * z + z * z * z * z);
}

double projectEnvironment(const EnvironmentParams& params, float* outCoefficientsRgb)
{
    const double startSeconds = nowSeconds();

    const int thetaSteps = 256;
    const int phiSteps = 512;

    double coefficients[SH_MAX_COEFFICIENTS * 3] = {};
    double basis[SH_MAX_COEFFICIENTS] = {};

    const double deltaTheta = PI / thetaSteps;
    const double deltaPhi = 2.0 * PI / phiSteps;

    for (int thetaIndex = 0; thetaIndex < thetaSteps; ++thetaIndex) {
        const double theta = (thetaIndex + 0.5) * deltaTheta;
        const double sinTheta = std::sin(theta);
        const double cosTheta = std::cos(theta);
        // 立体角里的 sinθ 因子在这里乘进来
        const double thetaWeight = sinTheta * deltaTheta * deltaPhi;
        for (int phiIndex = 0; phiIndex < phiSteps; ++phiIndex) {
            const double phi = (phiIndex + 0.5) * deltaPhi;
            const glm::dvec3 direction(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));

            const glm::vec3 radiance = environmentRadiance(glm::vec3(direction), params);
            evaluateShBasis(direction, basis);

            for (int i = 0; i < SH_MAX_COEFFICIENTS; ++i) {
                const double weight = basis[i] * thetaWeight;
                coefficients[i * 3 + 0] += static_cast<double>(radiance.r) * weight;
                coefficients[i * 3 + 1] += static_cast<double>(radiance.g) * weight;
                coefficients[i * 3 + 2] += static_cast<double>(radiance.b) * weight;
            }
        }
    }

    for (int i = 0; i < SH_MAX_COEFFICIENTS * 3; ++i) {
        outCoefficientsRgb[i] = static_cast<float>(coefficients[i]);
    }
    return nowSeconds() - startSeconds;
}

// angleRadians 是环境自身的旋转角，与着色器里把采样方向转回原始坐标系时用的角度取反。
// 基函数在 m 为正时正比于 cos(mφ)、为负时正比于 sin(mφ)，因此这两个分量按角度 m 倍旋转
void rotateShAboutY(float* coefficientsRgb, uint32_t bandCount, float angleRadians)
{
    const uint32_t bands = std::min<uint32_t>(bandCount, SH_MAX_BANDS);
    for (uint32_t band = 1; band < bands; ++band) {
        for (uint32_t m = 1; m <= band; ++m) {
            const int positiveIndex = static_cast<int>(band * band + band + m);
            const int negativeIndex = static_cast<int>(band * band + band - m);
            const float cosine = std::cos(static_cast<float>(m) * angleRadians);
            const float sine = std::sin(static_cast<float>(m) * angleRadians);
            for (int channel = 0; channel < 3; ++channel) {
                const float positive = coefficientsRgb[positiveIndex * 3 + channel];
                const float negative = coefficientsRgb[negativeIndex * 3 + channel];
                coefficientsRgb[positiveIndex * 3 + channel] = positive * cosine - negative * sine;
                coefficientsRgb[negativeIndex * 3 + channel] = negative * cosine + positive * sine;
            }
        }
    }
}

// Ramamoorthi 给出的漫反射卷积因子，奇数阶里的三阶为零
static double shConvolutionFactor(int band)
{
    static const double factors[SH_MAX_BANDS] = {
        3.14159265358979,   // l = 0
        2.09439510239320,   // l = 1
        0.785398163397448,  // l = 2
        0.0,                // l = 3
        -0.130899693899575, // l = 4
    };
    return factors[band];
}

static int shBandOfIndex(int index)
{
    int band = 0;
    while ((band + 1) * (band + 1) <= index) {
        ++band;
    }
    return band;
}

glm::vec3 shIrradiance(const float* coefficientsRgb, uint32_t bandCount, const glm::vec3& normal)
{
    const glm::dvec3 d = glm::normalize(glm::dvec3(normal));
    double basis[SH_MAX_COEFFICIENTS] = {};
    evaluateShBasis(d, basis);

    const uint32_t bands = std::min<uint32_t>(bandCount, SH_MAX_BANDS);
    double irradiance[3] = {};
    for (uint32_t i = 0; i < bands * bands; ++i) {
        const double factor = basis[i] * shConvolutionFactor(shBandOfIndex(static_cast<int>(i)));
        for (int channel = 0; channel < 3; ++channel) {
            irradiance[channel] += factor * static_cast<double>(coefficientsRgb[i * 3 + channel]);
        }
    }

    return glm::vec3(static_cast<float>(irradiance[0]), static_cast<float>(irradiance[1]),
                     static_cast<float>(irradiance[2]));
}

void buildUvSphereMesh(uint32_t segments, float radius, MeshData& outMesh)
{
    outMesh = MeshData();

    const uint32_t longitudeSegments = std::max(4u, segments);
    const uint32_t latitudeSegments = std::max(2u, segments / 2);
    const float pi = 3.14159265358979323846f;

    for (uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude) {
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float theta = v * pi;
        for (uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude) {
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float phi = u * 2.0f * pi;

            const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta),
                                   std::sin(theta) * std::sin(phi));
            outMesh.vertices.push_back({ normal * radius, normal, glm::vec2(u, v) });
        }
    }

    const uint32_t stride = longitudeSegments + 1;
    for (uint32_t latitude = 0; latitude < latitudeSegments; ++latitude) {
        for (uint32_t longitude = 0; longitude < longitudeSegments; ++longitude) {
            const uint32_t topLeft = latitude * stride + longitude;
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + stride;
            const uint32_t bottomRight = bottomLeft + 1;

            if (latitude > 0) {
                outMesh.indices.push_back(topLeft);
                outMesh.indices.push_back(topRight);
                outMesh.indices.push_back(bottomRight);
            }
            if (latitude + 1 < latitudeSegments) {
                outMesh.indices.push_back(topLeft);
                outMesh.indices.push_back(bottomRight);
                outMesh.indices.push_back(bottomLeft);
            }
        }
    }

    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = radius;
}
