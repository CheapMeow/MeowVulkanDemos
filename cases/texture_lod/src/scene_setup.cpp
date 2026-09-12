#include "scene_setup.h"

#include <cmath>

void buildGroundQuadMesh(float halfExtent, MeshData& outMesh)
{
    outMesh = MeshData();
    outMesh.vertices = {
        { glm::vec3(-halfExtent, 0.0f, -halfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f) },
        { glm::vec3(halfExtent, 0.0f, -halfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f) },
        { glm::vec3(halfExtent, 0.0f, halfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f) },
        { glm::vec3(-halfExtent, 0.0f, halfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f) },
    };
    outMesh.indices = { 0, 1, 2, 0, 2, 3 };
    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = halfExtent * 1.41421356f;
}

// 廉价的确定性噪声，避免引入随机数生成器的状态
static float hashNoise(uint32_t x, uint32_t y)
{
    uint32_t value = x * 374761393u + y * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    value = value ^ (value >> 16);
    return static_cast<float>(value & 0xFFFFu) / 65535.0f;
}

void buildDetailTexturePixels(uint32_t size, std::vector<unsigned char>& outPixels)
{
    outPixels.resize(static_cast<size_t>(size) * size * 4);

    const uint32_t checkerSize = 8;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t cellX = x / checkerSize;
            const uint32_t cellY = y / checkerSize;
            const bool bright = ((cellX + cellY) & 1u) != 0;

            // 棋盘格上叠一层细噪声，缩小之后仍有可见的平均色差
            const float noise = 0.75f + 0.5f * hashNoise(x, y) - 0.25f;
            const float base = bright ? 0.82f : 0.18f;
            const float value = base * noise;

            const unsigned char encoded = static_cast<unsigned char>(value * 255.0f + 0.5f);
            const size_t index = (static_cast<size_t>(y) * size + x) * 4;
            outPixels[index + 0] = encoded;
            outPixels[index + 1] = encoded;
            outPixels[index + 2] = encoded;
            outPixels[index + 3] = 255;
        }
    }
}
