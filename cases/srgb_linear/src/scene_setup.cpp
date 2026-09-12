#include "scene_setup.h"

#include <glm/glm.hpp>

const std::vector<float>& referenceBarValues()
{
    // 0.5 是 sRGB 曲线中段最常被引用的值，0.18 是常见的中灰，另外两个取更暗的一档
    static const std::vector<float> values = { 0.5f, 0.35f, 0.18f, 0.05f };
    return values;
}

void buildReferenceBarMesh(const std::vector<float>& values, MeshData& outMesh)
{
    outMesh = MeshData();

    const float patchWidth = 0.16f;
    const float patchHeight = 0.12f;
    const float gap = 0.02f;
    const float left = -0.96f;
    const float bottom = -0.96f;

    for (size_t i = 0; i < values.size(); ++i) {
        const float x0 = left + static_cast<float>(i) * (patchWidth + gap);
        const float x1 = x0 + patchWidth;
        const float y0 = bottom;
        const float y1 = bottom + patchHeight;
        const float value = values[i];

        const uint32_t base = static_cast<uint32_t>(outMesh.vertices.size());
        outMesh.vertices.push_back({ glm::vec3(x0, y0, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back({ glm::vec3(x1, y0, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back({ glm::vec3(x1, y1, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back({ glm::vec3(x0, y1, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });

        outMesh.indices.push_back(base + 0);
        outMesh.indices.push_back(base + 1);
        outMesh.indices.push_back(base + 2);
        outMesh.indices.push_back(base + 0);
        outMesh.indices.push_back(base + 2);
        outMesh.indices.push_back(base + 3);
    }

    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = 1.0f;
}
