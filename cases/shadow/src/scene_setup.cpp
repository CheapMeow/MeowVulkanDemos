#include "scene_setup.h"

#include <algorithm>
#include <cmath>

struct InstanceSortEntry {
    float distanceToCenter;
    uint32_t gridIndex;
};

static bool compareByDistanceToCenter(const InstanceSortEntry& left, const InstanceSortEntry& right)
{
    if (left.distanceToCenter != right.distanceToCenter) {
        return left.distanceToCenter < right.distanceToCenter;
    }
    return left.gridIndex < right.gridIndex;
}

void buildShadowInstances(uint32_t instanceCount, float spacing, float objectCenterHeight, float scale,
                          std::vector<InstanceData>& outInstances)
{
    const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(instanceCount))));
    const float gridCenter = 0.5f * static_cast<float>(gridSide - 1);

    std::vector<InstanceSortEntry> sortEntries(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
        const float offsetX = static_cast<float>(i % gridSide) - gridCenter;
        const float offsetZ = static_cast<float>(i / gridSide) - gridCenter;
        sortEntries[i].distanceToCenter = offsetX * offsetX + offsetZ * offsetZ;
        sortEntries[i].gridIndex = i;
    }
    std::sort(sortEntries.begin(), sortEntries.end(), compareByDistanceToCenter);

    const float gridOrigin = -gridCenter * spacing;

    outInstances.clear();
    outInstances.reserve(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
        const uint32_t gridIndex = sortEntries[i].gridIndex;
        const uint32_t gridX = gridIndex % gridSide;
        const uint32_t gridZ = gridIndex / gridSide;

        InstanceData instance;
        instance.positionScale =
            glm::vec4(gridOrigin + static_cast<float>(gridX) * spacing, objectCenterHeight * scale,
                      gridOrigin + static_cast<float>(gridZ) * spacing, scale);
        instance.rotation = glm::vec4(static_cast<float>(gridIndex) * 0.37f, 0.0f, 0.0f, 0.0f);
        outInstances.push_back(instance);
    }
}

float shadowGridHalfExtent(uint32_t instanceCount, float spacing)
{
    if (instanceCount <= 1) {
        return 0.0f;
    }
    const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(instanceCount))));
    return 0.5f * static_cast<float>(gridSide - 1) * spacing;
}

float objectCenterHeightForMesh(const MeshData& mesh)
{
    float lowest = mesh.vertices[0].position.y;
    for (size_t i = 1; i < mesh.vertices.size(); ++i) {
        lowest = std::min(lowest, mesh.vertices[i].position.y);
    }
    return -lowest;
}

void buildGroundPlaneMesh(MeshData& outMesh)
{
    outMesh = MeshData();
    outMesh.vertices = {
        { glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f) },
        { glm::vec3(0.5f, 0.0f, -0.5f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f) },
        { glm::vec3(0.5f, 0.0f, 0.5f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f) },
        { glm::vec3(-0.5f, 0.0f, 0.5f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f) },
    };
    outMesh.indices = { 0, 1, 2, 0, 2, 3 };
    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = 0.70710678f;
}
