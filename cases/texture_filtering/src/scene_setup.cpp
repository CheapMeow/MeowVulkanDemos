#include "scene_setup.h"

#include <algorithm>
#include <cmath>

static const float PI = 3.14159265358979323846f;

// 两个错开的频率渐变叠加，三个通道用不同的相位。整个图案是带限的：
// 最高频率出现在离中心最远的地方，约为八分之一纹素一个周期，纹理本身采样得动
glm::vec3 patternColor(const glm::vec2& uv)
{
    const glm::vec2 first = uv - glm::vec2(0.38f, 0.42f);
    const glm::vec2 second = uv - glm::vec2(0.66f, 0.60f);
    const float firstPhase = glm::dot(first, first) * 4.0f * 70.0f;
    const float secondPhase = glm::dot(second, second) * 4.0f * 70.0f;

    return glm::vec3(0.5f + 0.5f * std::cos(firstPhase),
                     0.5f + 0.5f * std::cos(firstPhase * 1.05f + 1.0f),
                     0.5f + 0.5f * std::cos(secondPhase * 1.10f + 2.0f));
}

float patternValue(const glm::vec2& uv)
{
    const glm::vec3 color = patternColor(uv);
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

std::vector<unsigned char> buildPatternPixels()
{
    std::vector<unsigned char> pixels(static_cast<size_t>(PATTERN_TEXTURE_SIZE) *
                                          PATTERN_TEXTURE_SIZE * 4,
                                      255);

    for (int y = 0; y < PATTERN_TEXTURE_SIZE; ++y) {
        for (int x = 0; x < PATTERN_TEXTURE_SIZE; ++x) {
            // 取格子中心，避免纹理自身的采样位置偏移
            const glm::vec2 uv((static_cast<float>(x) + 0.5f) / PATTERN_TEXTURE_SIZE,
                               (static_cast<float>(y) + 0.5f) / PATTERN_TEXTURE_SIZE);
            const glm::vec3 color = glm::clamp(patternColor(uv), glm::vec3(0.0f), glm::vec3(1.0f));
            const size_t index = (static_cast<size_t>(y) * PATTERN_TEXTURE_SIZE + x) * 4;
            pixels[index + 0] = static_cast<unsigned char>(std::lround(color.r * 255.0f));
            pixels[index + 1] = static_cast<unsigned char>(std::lround(color.g * 255.0f));
            pixels[index + 2] = static_cast<unsigned char>(std::lround(color.b * 255.0f));
        }
    }

    return pixels;
}
