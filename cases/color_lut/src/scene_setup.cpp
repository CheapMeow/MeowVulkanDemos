#include "scene_setup.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

// 与着色器里同名函数保持一致：先做一条 S 曲线，再按亮度把色温从冷推到暖，最后按亮度调整饱和度
glm::vec3 gradeReference(const glm::vec3& color)
{
    const glm::vec3 clamped = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));

    glm::vec3 s = clamped * clamped * (3.0f - 2.0f * clamped);
    s = glm::mix(clamped, s, 0.6f);

    const float luminance = glm::dot(s, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const glm::vec3 warm(1.05f, 1.0f, 0.92f);
    const glm::vec3 cool(0.92f, 0.97f, 1.10f);
    s *= glm::mix(cool, warm, glm::smoothstep(0.2f, 0.8f, luminance));

    const float saturation = glm::mix(0.55f, 1.10f, glm::smoothstep(0.05f, 0.45f, luminance));
    const float grey = glm::dot(s, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    s = glm::mix(glm::vec3(grey), s, saturation);

    return glm::clamp(s, glm::vec3(0.0f), glm::vec3(1.0f));
}

std::vector<unsigned char> buildThreeDimensionalLut(uint32_t size)
{
    const uint32_t width = size * size;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * size * 4, 255);

    for (uint32_t blue = 0; blue < size; ++blue) {
        for (uint32_t green = 0; green < size; ++green) {
            for (uint32_t red = 0; red < size; ++red) {
                const float r = static_cast<float>(red) / static_cast<float>(size - 1);
                const float g = static_cast<float>(green) / static_cast<float>(size - 1);
                const float b = static_cast<float>(blue) / static_cast<float>(size - 1);
                const glm::vec3 graded = gradeReference(glm::vec3(r, g, b));

                const uint32_t x = blue * size + red;
                const uint32_t y = green;
                const size_t index = (static_cast<size_t>(y) * width + x) * 4;
                pixels[index + 0] = static_cast<unsigned char>(std::lround(graded.r * 255.0f));
                pixels[index + 1] = static_cast<unsigned char>(std::lround(graded.g * 255.0f));
                pixels[index + 2] = static_cast<unsigned char>(std::lround(graded.b * 255.0f));
            }
        }
    }

    return pixels;
}

std::vector<unsigned char> buildOneDimensionalLut()
{
    std::vector<unsigned char> pixels(static_cast<size_t>(GRADE_ONE_DIMENSIONAL_SIZE) * 4, 255);

    for (uint32_t i = 0; i < GRADE_ONE_DIMENSIONAL_SIZE; ++i) {
        const float value = static_cast<float>(i) / static_cast<float>(GRADE_ONE_DIMENSIONAL_SIZE - 1);
        const glm::vec3 graded = gradeReference(glm::vec3(value));
        pixels[i * 4 + 0] = static_cast<unsigned char>(std::lround(graded.r * 255.0f));
        pixels[i * 4 + 1] = static_cast<unsigned char>(std::lround(graded.g * 255.0f));
        pixels[i * 4 + 2] = static_cast<unsigned char>(std::lround(graded.b * 255.0f));
    }

    return pixels;
}

static void buildUvSphere(float radius, const glm::vec3& center, const glm::vec3& color,
                          uint32_t segments, std::vector<ColorVertex>& outVertices,
                          std::vector<uint32_t>& outIndices)
{
    const uint32_t longitudeSegments = segments;
    const uint32_t latitudeSegments = std::max(2u, segments / 2);
    const float pi = 3.14159265358979323846f;
    const uint32_t base = static_cast<uint32_t>(outVertices.size());

    for (uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude) {
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float theta = v * pi;
        for (uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude) {
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float phi = u * 2.0f * pi;
            const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta),
                                   std::sin(theta) * std::sin(phi));
            outVertices.push_back({ center + normal * radius, normal, color });
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

void buildSphereRow(std::vector<ColorVertex>& outVertices, std::vector<uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();

    // 七个高饱和度的球，色相分布在整个色轮上，调色对色相的影响会直接显出来
    static const glm::vec3 colors[7] = {
        glm::vec3(0.85f, 0.15f, 0.12f), glm::vec3(0.90f, 0.50f, 0.10f),
        glm::vec3(0.88f, 0.85f, 0.18f), glm::vec3(0.18f, 0.72f, 0.25f),
        glm::vec3(0.15f, 0.42f, 0.88f), glm::vec3(0.65f, 0.20f, 0.78f),
        glm::vec3(0.55f, 0.55f, 0.55f),
    };

    const float spacing = 1.35f;
    for (uint32_t i = 0; i < 7; ++i) {
        const float x = (static_cast<float>(i) - 3.0f) * spacing;
        buildUvSphere(0.62f, glm::vec3(x, 0.0f, 0.0f), colors[i], 32, outVertices, outIndices);
    }
}

void buildReferencePatches(std::vector<ColorVertex>& outVertices, std::vector<uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();

    const float patchWidth = 0.055f;
    const float patchHeight = 0.10f;
    const float gap = 0.006f;
    const float bottom = -0.97f;

    // 十六级灰阶写线性辐射亮度，从 0.02 到 1.6，经过色调映射之后覆盖整个显示范围
    for (uint32_t i = 0; i < 16; ++i) {
        const float t = static_cast<float>(i) / 15.0f;
        const float value = 0.02f * std::pow(80.0f, t);
        const glm::vec3 color(value);
        const float x0 = -0.97f + static_cast<float>(i) * (patchWidth + gap);
        const float x1 = x0 + patchWidth;
        const uint32_t base = static_cast<uint32_t>(outVertices.size());

        outVertices.push_back({ glm::vec3(x0, bottom, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), color });
        outVertices.push_back({ glm::vec3(x1, bottom, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), color });
        outVertices.push_back({ glm::vec3(x1, bottom + patchHeight, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), color });
        outVertices.push_back({ glm::vec3(x0, bottom + patchHeight, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), color });

        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
    }

    // 八块饱和色，用来暴露每通道一维曲线做不到的色相相关变换
    static const glm::vec3 patchColors[8] = {
        glm::vec3(0.60f, 0.10f, 0.08f), glm::vec3(0.60f, 0.32f, 0.06f),
        glm::vec3(0.58f, 0.55f, 0.10f), glm::vec3(0.10f, 0.42f, 0.14f),
        glm::vec3(0.08f, 0.26f, 0.58f), glm::vec3(0.42f, 0.12f, 0.52f),
        glm::vec3(0.35f, 0.35f, 0.35f), glm::vec3(0.75f, 0.75f, 0.75f),
    };

    for (uint32_t i = 0; i < 8; ++i) {
        const float x0 = 0.20f + static_cast<float>(i) * (patchWidth + gap);
        const float x1 = x0 + patchWidth;
        const uint32_t base = static_cast<uint32_t>(outVertices.size());

        outVertices.push_back({ glm::vec3(x0, bottom, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), patchColors[i] });
        outVertices.push_back({ glm::vec3(x1, bottom, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), patchColors[i] });
        outVertices.push_back(
            { glm::vec3(x1, bottom + patchHeight, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), patchColors[i] });
        outVertices.push_back(
            { glm::vec3(x0, bottom + patchHeight, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), patchColors[i] });

        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
    }
}
