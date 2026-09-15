#include "scene_setup.h"

#include <glm/glm.hpp>

const std::vector<float>& referenceBarValues()
{
    // 从全黑到 64，每格是前一格的两倍；0.18 附近的中灰与 1.0 附近的显示器白点都在其中
    static const std::vector<float> values = {
        0.0f,    0.015625f, 0.03125f, 0.0625f, 0.125f, 0.25f,   0.5f,
        1.0f,    2.0f,      4.0f,     8.0f,    16.0f,  32.0f,  64.0f,
    };
    return values;
}

void buildReferenceBarMesh(const std::vector<float>& values, MeshData& outMesh)
{
    outMesh = MeshData();

    const float patchWidth = 0.12f;
    const float patchHeight = 0.10f;
    const float gap = 0.01f;
    const float left = -0.97f;
    const float bottom = -0.97f;

    for (size_t i = 0; i < values.size(); ++i) {
        const float x0 = left + static_cast<float>(i) * (patchWidth + gap);
        const float x1 = x0 + patchWidth;
        const float y0 = bottom;
        const float y1 = bottom + patchHeight;
        const float value = values[i];

        const uint32_t base = static_cast<uint32_t>(outMesh.vertices.size());
        outMesh.vertices.push_back(
            { glm::vec3(x0, y0, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back(
            { glm::vec3(x1, y0, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back(
            { glm::vec3(x1, y1, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });
        outMesh.vertices.push_back(
            { glm::vec3(x0, y1, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(value, 0.0f) });

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
