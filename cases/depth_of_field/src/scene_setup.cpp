#include "scene_setup.h"

#include <algorithm>
#include <cmath>

static const float PI = 3.14159265358979323846f;
static const uint32_t SPHERE_SEGMENTS = 24;

static void buildUvSphere(float radius, const glm::vec3& center, const glm::vec3& color,
                          float emission, std::vector<SceneVertex>& outVertices,
                          std::vector<uint32_t>& outIndices)
{
    const uint32_t latitudeSegments = std::max(2u, SPHERE_SEGMENTS / 2);
    const uint32_t longitudeSegments = SPHERE_SEGMENTS;
    const uint32_t base = static_cast<uint32_t>(outVertices.size());

    for (uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude) {
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float theta = v * PI;
        for (uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude) {
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float phi = u * 2.0f * PI;
            const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta),
                                   std::sin(theta) * std::sin(phi));
            outVertices.push_back({ center + normal * radius, normal, color, emission });
        }
    }

    const uint32_t stride = longitudeSegments + 1;
    for (uint32_t latitude = 0; latitude < latitudeSegments; ++latitude) {
        for (uint32_t longitude = 0; longitude < longitudeSegments; ++longitude) {
            const uint32_t topLeft = base + latitude * stride + longitude;
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + stride;
            const uint32_t bottomRight = bottomLeft + 1;

            if (latitude > 0) {
                outIndices.push_back(topLeft);
                outIndices.push_back(topRight);
                outIndices.push_back(bottomRight);
            }
            if (latitude + 1 < latitudeSegments) {
                outIndices.push_back(topLeft);
                outIndices.push_back(bottomRight);
                outIndices.push_back(bottomLeft);
            }
        }
    }
}

static void pushQuad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
                     const glm::vec3& normal, const glm::vec3& color, std::vector<SceneVertex>& outVertices,
                     std::vector<uint32_t>& outIndices)
{
    const uint32_t base = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back({ a, normal, color, 0.0f });
    outVertices.push_back({ b, normal, color, 0.0f });
    outVertices.push_back({ c, normal, color, 0.0f });
    outVertices.push_back({ d, normal, color, 0.0f });
    outIndices.push_back(base + 0);
    outIndices.push_back(base + 1);
    outIndices.push_back(base + 2);
    outIndices.push_back(base + 0);
    outIndices.push_back(base + 2);
    outIndices.push_back(base + 3);
}

void buildScene(std::vector<SceneVertex>& outVertices, std::vector<uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();

    // 地面从相机脚下铺到远处，深度跨度六个单位
    const float left = -8.0f;
    const float right = 8.0f;
    const float near = 0.4f;
    const float far = -14.0f;
    const glm::vec3 groundColor(0.10f, 0.11f, 0.13f);
    pushQuad(glm::vec3(left, -0.6f, near), glm::vec3(right, -0.6f, near),
             glm::vec3(right, -0.6f, far), glm::vec3(left, -0.6f, far), glm::vec3(0.0f, 1.0f, 0.0f),
             groundColor, outVertices, outIndices);

    // 五排小亮球，深度从 1.2 拉到 6，用来观察不同深度上的散景形状
    const glm::vec3 glowColor(1.0f, 0.85f, 0.55f);
    for (int row = 0; row < 5; ++row) {
        const float z = -1.2f - static_cast<float>(row) * 1.2f;
        for (int column = 0; column < 5; ++column) {
            const float x = (static_cast<float>(column) - 2.0f) * 0.5f;
            const float radius = 0.055f - static_cast<float>(row) * 0.004f;
            buildUvSphere(radius, glm::vec3(x, -0.6f + radius, z), glowColor, 20.0f, outVertices,
                          outIndices);
        }
    }

    // 前景与背景各放一个大球，前者靠近对焦距离，后者远在焦外
    buildUvSphere(0.22f, glm::vec3(0.66f, -0.30f, -1.05f), glm::vec3(0.75f, 0.35f, 0.30f), 0.0f,
                  outVertices, outIndices);
    buildUvSphere(0.45f, glm::vec3(-1.05f, -0.12f, -5.6f), glm::vec3(0.35f, 0.55f, 0.80f), 0.0f,
                  outVertices, outIndices);
}

float circleOfConfusionPixels(float viewDistance, float focalLength, float fNumber,
                             float focusDistance, float sensorWidth, float imageWidth)
{
    // 薄透镜：弥散圆直径 c = f^2 (z - z_f) / (N z (z_f - f))，z 是物距
    const float denominator = fNumber * viewDistance * (focusDistance - focalLength);
    if (std::fabs(denominator) < 1e-6f) {
        return 0.0f;
    }
    const float diameter =
        focalLength * focalLength * (viewDistance - focusDistance) / denominator;
    // 由成像面上的尺寸换算到像素半径
    return 0.5f * diameter / sensorWidth * imageWidth;
}
