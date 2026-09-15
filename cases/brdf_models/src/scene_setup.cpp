#include "scene_setup.h"

#include "timing.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

static const double PI = 3.14159265358979323846;

// 在半球上按各个法线分布自身的重要性采样取一个半程向量，u1 与 u2 是两个均匀随机数
static void sampleHalfVector(double u1, double u2, double roughness, uint32_t distribution,
                             double* outHalf)
{
    const double alpha = roughness * roughness;
    const double alpha2 = alpha * alpha;

    double cosine = 1.0;
    if (distribution == BRDF_DISTRIBUTION_BECKMANN) {
        const double tangent2 = -alpha2 * std::log(std::max(1.0 - u2, 1e-8));
        cosine = 1.0 / std::sqrt(1.0 + tangent2);
    } else if (distribution == BRDF_DISTRIBUTION_BLINN_PHONG) {
        const double power = 2.0 / std::max(alpha2, 1e-6) - 2.0;
        cosine = std::pow(u2, 1.0 / (power + 1.0));
    } else {
        cosine = std::sqrt(std::max((1.0 - u2) / (1.0 + (alpha2 - 1.0) * u2), 0.0));
    }

    const double sine = std::sqrt(std::max(1.0 - cosine * cosine, 0.0));
    const double phi = 2.0 * PI * u1;
    outHalf[0] = sine * std::cos(phi);
    outHalf[1] = sine * std::sin(phi);
    outHalf[2] = cosine;
}

// 可见性项 V = G / (4 (n·v)(n·l))，高度相关的 Smith 形式，微表面 BRDF 里直接用 D V F
static double smithVisibility(double nDotV, double nDotL, double roughness)
{
    const double alpha = roughness * roughness;
    const double alpha2 = alpha * alpha;

    const double lambdaV = nDotL * std::sqrt(nDotV * nDotV * (1.0 - alpha2) + alpha2);
    const double lambdaL = nDotV * std::sqrt(nDotL * nDotL * (1.0 - alpha2) + alpha2);
    return 0.5 / std::max(lambdaV + lambdaL, 1e-8);
}

static double schlickFresnel(double cosine, double reflectance)
{
    const double f0 = reflectance;
    return f0 + (1.0 - f0) * std::pow(1.0 - cosine, 5.0);
}

// 与着色器里同一套低差异序列，建表与片上积分用同样的采样点，两边结果才对得上
static double radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

BrdfTableResult buildBrdfTables(uint32_t distribution, float reflectance, uint32_t sampleCount)
{
    const double startSeconds = nowSeconds();

    BrdfTableResult result;
    result.directionalAlbedo.resize(BRDF_TABLE_ROUGHNESS_COUNT * BRDF_TABLE_COSINE_COUNT, 0.0f);
    result.averageAlbedo.resize(BRDF_AVERAGE_COUNT, 0.0f);

    for (int roughnessIndex = 0; roughnessIndex < BRDF_TABLE_ROUGHNESS_COUNT; ++roughnessIndex) {
        // 粗糙度取格子中心，第一格留下一点值避免除零
        const double roughness =
            std::max((roughnessIndex + 0.5) / BRDF_TABLE_ROUGHNESS_COUNT, 0.02);

        for (int cosineIndex = 0; cosineIndex < BRDF_TABLE_COSINE_COUNT; ++cosineIndex) {
            // 余弦方向取到两个端点，保证 μ 等于 1 与 0 都有格子，查询时直接线性插值
            const double mu =
                static_cast<double>(cosineIndex) / (BRDF_TABLE_COSINE_COUNT - 1);
            const double sine = std::sqrt(std::max(1.0 - mu * mu, 0.0));
            const double view[3] = { 0.0, sine, mu };

            double sum = 0.0;
            for (uint32_t sample = 0; sample < sampleCount; ++sample) {
                const double u1 = (static_cast<double>(sample) + 0.5) / sampleCount;
                const double u2 = radicalInverse(sample);

                double half[3] = {};
                sampleHalfVector(u1, u2, roughness, distribution, half);

                const double vDotH = view[0] * half[0] + view[1] * half[1] + view[2] * half[2];
                if (vDotH <= 0.0) {
                    continue;
                }

                // 由半程向量反射出光线
                const double light[3] = {
                    2.0 * vDotH * half[0] - view[0],
                    2.0 * vDotH * half[1] - view[1],
                    2.0 * vDotH * half[2] - view[2],
                };
                const double nDotL = light[2];
                if (nDotL <= 0.0) {
                    continue;
                }

                const double nDotH = half[2];
                const double visibility = smithVisibility(mu, nDotL, roughness);
                const double fresnel = schlickFresnel(vDotH, reflectance);

                // 微表面 BRDF 乘上 n·l 再除以采样概率。采样概率是 D(h)(n·h)/(4(v·h))，
                // 与 BRDF 里的 D 约掉之后剩下 4 V F (v·h)(n·l)/(n·h)
                sum += 4.0 * visibility * fresnel * vDotH * nDotL / std::max(nDotH, 1e-8);
            }

            result.directionalAlbedo[roughnessIndex * BRDF_TABLE_COSINE_COUNT + cosineIndex] =
                static_cast<float>(sum / static_cast<double>(sampleCount));
        }
    }

    // E_avg = 2 ∫ E(μ) μ dμ，两端取到 0 与 1，用梯形法求和
    const double step = 1.0 / (BRDF_TABLE_COSINE_COUNT - 1);
    for (int roughnessIndex = 0; roughnessIndex < BRDF_TABLE_ROUGHNESS_COUNT; ++roughnessIndex) {
        double average = 0.0;
        for (int cosineIndex = 0; cosineIndex < BRDF_TABLE_COSINE_COUNT; ++cosineIndex) {
            const double mu = static_cast<double>(cosineIndex) / (BRDF_TABLE_COSINE_COUNT - 1);
            const double e =
                result.directionalAlbedo[roughnessIndex * BRDF_TABLE_COSINE_COUNT + cosineIndex];
            const double weight = (cosineIndex == 0 || cosineIndex + 1 == BRDF_TABLE_COSINE_COUNT)
                                      ? 0.5 * step
                                      : step;
            average += 2.0 * e * mu * weight;
        }
        result.averageAlbedo[roughnessIndex] = static_cast<float>(average);
    }

    result.seconds = nowSeconds() - startSeconds;
    return result;
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
