#include "scene_setup.h"

#include <algorithm>
#include <cmath>

void buildGroundStripMesh(MeshData& outMesh)
{
    outMesh = MeshData();

    // 每段的下一个距离按固定比例放大。近处分段密，远处单个分段覆盖的范围随距离增长，
    // 这样固定段数就能覆盖从贴着相机到很远的整个深度范围
    const float distanceRatio =
        std::pow(GROUND_END_DISTANCE / GROUND_START_DISTANCE, 1.0f / static_cast<float>(GROUND_SEGMENT_COUNT));

    outMesh.vertices.reserve(2 * (GROUND_SEGMENT_COUNT + 1));
    float distance = GROUND_START_DISTANCE;
    for (int ring = 0; ring <= GROUND_SEGMENT_COUNT; ++ring) {
        const float halfWidth = distance * GROUND_HALF_WIDTH_RATIO;
        outMesh.vertices.push_back({ glm::vec3(-halfWidth, 0.0f, -distance), glm::vec3(0.0f, 1.0f, 0.0f),
                                     glm::vec2(0.0f, distance) });
        outMesh.vertices.push_back({ glm::vec3(halfWidth, 0.0f, -distance), glm::vec3(0.0f, 1.0f, 0.0f),
                                     glm::vec2(1.0f, distance) });
        distance *= distanceRatio;
    }

    outMesh.indices.reserve(6 * GROUND_SEGMENT_COUNT);
    for (int ring = 0; ring < GROUND_SEGMENT_COUNT; ++ring) {
        const uint32_t nearLeft = static_cast<uint32_t>(2 * ring);
        const uint32_t nearRight = nearLeft + 1;
        const uint32_t farLeft = nearLeft + 2;
        const uint32_t farRight = nearLeft + 3;

        outMesh.indices.push_back(nearLeft);
        outMesh.indices.push_back(nearRight);
        outMesh.indices.push_back(farRight);

        outMesh.indices.push_back(nearLeft);
        outMesh.indices.push_back(farRight);
        outMesh.indices.push_back(farLeft);
    }

    // 包围球按最远的一端估算，本 case 不做视锥剔除，只用它描述网格范围
    outMesh.boundsCenter = glm::vec3(0.0f);
    outMesh.boundsRadius = GROUND_END_DISTANCE * GROUND_HALF_WIDTH_RATIO;
}

float objectCenterHeightForMesh(const MeshData& mesh)
{
    float lowest = mesh.vertices[0].position.y;
    for (size_t i = 1; i < mesh.vertices.size(); ++i) {
        lowest = std::min(lowest, mesh.vertices[i].position.y);
    }
    return -lowest;
}

void buildObjectInstances(uint32_t instanceCount, float objectCenterHeight,
                          std::vector<InstanceData>& outInstances)
{
    outInstances.clear();
    outInstances.reserve(instanceCount);

    for (uint32_t i = 0; i < instanceCount; ++i) {
        const uint32_t row = i / 2;
        const bool leftColumn = (i % 2) == 0;

        const float distance = OBJECT_FIRST_DISTANCE * std::pow(OBJECT_DISTANCE_RATIO, static_cast<float>(row));
        const float lateral = (leftColumn ? -1.0f : 1.0f) * distance * OBJECT_LATERAL_RATIO;
        const float scale = distance * OBJECT_SCALE_RATIO;

        InstanceData instance;
        // 物体底面落在 y = 0 的地面上，两层地面的间距远小于物体高度，视觉上不构成差异
        instance.positionScale = glm::vec4(lateral, objectCenterHeight * scale, -distance, scale);
        instance.rotation = glm::vec4(static_cast<float>(i) * 0.9f, 0.0f, 0.0f, 0.0f);
        outInstances.push_back(instance);
    }
}

void buildGroundInstances(std::vector<InstanceData>& outInstances)
{
    outInstances.clear();
    outInstances.reserve(2);

    for (uint32_t layer = 0; layer < 2; ++layer) {
        InstanceData instance;
        instance.positionScale = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        instance.rotation = glm::vec4(0.0f, static_cast<float>(layer), 0.0f, 0.0f);
        outInstances.push_back(instance);
    }
}
