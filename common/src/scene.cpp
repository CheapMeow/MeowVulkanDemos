#include "scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

// 排序用的中间记录
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

void buildInstances(uint32_t instanceCount, float spacing, std::vector<InstanceData>& outInstances)
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
        const float x = gridOrigin + static_cast<float>(gridX) * spacing;
        const float z = gridOrigin + static_cast<float>(gridZ) * spacing;
        const float y = 8.0f * std::sin(static_cast<float>(gridX) * 0.7f) +
                        8.0f * std::cos(static_cast<float>(gridZ) * 0.5f);

        instance.positionScale = glm::vec4(x, y, z, 1.0f);
        instance.rotation = glm::vec4(static_cast<float>(gridIndex) * 0.37f, 0.0f, 0.0f, 0.0f);
        outInstances.push_back(instance);
    }
}

void updateLights(const glm::vec3& cameraPosition, uint32_t lightCount, float spacing, float range,
                  std::vector<LightData>& lights)
{
    const uint32_t gridSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(lightCount))));

    // 网格原点对齐到间距的整数倍，相机移动时光源位置不会抖动
    const float centerX = std::floor(cameraPosition.x / spacing) * spacing;
    const float centerZ = std::floor(cameraPosition.z / spacing) * spacing;
    const float halfExtent = 0.5f * static_cast<float>(gridSide - 1) * spacing;

    for (uint32_t i = 0; i < lightCount; ++i) {
        const uint32_t gridX = i % gridSide;
        const uint32_t gridZ = i / gridSide;

        const float x = centerX - halfExtent + static_cast<float>(gridX) * spacing;
        const float z = centerZ - halfExtent + static_cast<float>(gridZ) * spacing;

        // 颜色由网格整数坐标决定，相机移动时同一位置的光源颜色保持不变
        const int cellX = static_cast<int>(std::floor(x / spacing));
        const int cellZ = static_cast<int>(std::floor(z / spacing));
        const float hue = static_cast<float>(((cellX * 7 + cellZ * 13) % 16 + 16) % 16) / 16.0f * 6.2831853f;

        LightData light;
        light.positionRange = glm::vec4(x, 22.0f, z, range);
        light.color = glm::vec4(0.65f + 0.35f * std::sin(hue), 0.65f + 0.35f * std::sin(hue + 2.094f),
                                0.65f + 0.35f * std::sin(hue + 4.188f), 2400.0f);
        lights[i] = light;
    }
}

void initCamera(Camera& camera, const std::vector<InstanceData>& instances)
{
    camera = Camera();
    // 实例已按到网格中心的距离排序，第一个就是最靠近中心的
    const glm::vec3 gridCenter = glm::vec3(instances[0].positionScale);
    camera.position = gridCenter + glm::vec3(0.0f, 2.5f, 0.0f);
    camera.yaw = 0.785f;
    camera.pitch = 0.0f;
    camera.verticalFieldOfView = glm::radians(60.0f);
    camera.nearPlane = 0.5f;
    camera.farPlane = 160.0f;
    camera.moveSpeed = 40.0f;
}

glm::vec3 cameraForward(const Camera& camera)
{
    return glm::vec3(std::cos(camera.pitch) * std::sin(camera.yaw), std::sin(camera.pitch),
                     -std::cos(camera.pitch) * std::cos(camera.yaw));
}

void updateCamera(Camera& camera, const CameraInput& input, float deltaSeconds)
{
    const float rotateSpeed = 1.4f;
    if (input.turnLeft) {
        camera.yaw -= rotateSpeed * deltaSeconds;
    }
    if (input.turnRight) {
        camera.yaw += rotateSpeed * deltaSeconds;
    }
    if (input.turnUp) {
        camera.pitch += rotateSpeed * deltaSeconds;
    }
    if (input.turnDown) {
        camera.pitch -= rotateSpeed * deltaSeconds;
    }
    camera.pitch = glm::clamp(camera.pitch, -1.5f, 1.5f);

    const glm::vec3 forward = cameraForward(camera);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

    float speed = camera.moveSpeed;
    if (input.fast) {
        speed *= 4.0f;
    }

    if (input.moveForward) {
        camera.position += forward * speed * deltaSeconds;
    }
    if (input.moveBack) {
        camera.position -= forward * speed * deltaSeconds;
    }
    if (input.moveLeft) {
        camera.position -= right * speed * deltaSeconds;
    }
    if (input.moveRight) {
        camera.position += right * speed * deltaSeconds;
    }
    if (input.moveUp) {
        camera.position.y += speed * deltaSeconds;
    }
    if (input.moveDown) {
        camera.position.y -= speed * deltaSeconds;
    }
}

void fillCameraMatrices(const Camera& camera, float aspectRatio, CameraMatrices& outMatrices)
{
    const glm::vec3 forward = cameraForward(camera);
    outMatrices.view = glm::lookAt(camera.position, camera.position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    outMatrices.projection =
        glm::perspective(camera.verticalFieldOfView, aspectRatio, camera.nearPlane, camera.farPlane);
    // Vulkan 的裁剪空间 Y 轴朝下
    outMatrices.projection[1][1] *= -1.0f;
    outMatrices.viewProjection = outMatrices.projection * outMatrices.view;
    outMatrices.cameraPosition = glm::vec4(camera.position, 1.0f);
}

// Gribb-Hartmann 方法，从视图投影矩阵取出六个视锥平面
void extractFrustumPlanes(const glm::mat4& viewProjection, glm::vec4* outPlanes)
{
    const glm::vec4 rowX = glm::vec4(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0],
                                     viewProjection[3][0]);
    const glm::vec4 rowY = glm::vec4(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1],
                                     viewProjection[3][1]);
    const glm::vec4 rowZ = glm::vec4(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2],
                                     viewProjection[3][2]);
    const glm::vec4 rowW = glm::vec4(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3],
                                     viewProjection[3][3]);

    outPlanes[0] = rowW + rowX;
    outPlanes[1] = rowW - rowX;
    outPlanes[2] = rowW + rowY;
    outPlanes[3] = rowW - rowY;
    outPlanes[4] = rowZ;
    outPlanes[5] = rowW - rowZ;

    for (int i = 0; i < 6; ++i) {
        const float length = glm::length(glm::vec3(outPlanes[i]));
        outPlanes[i] /= length;
    }
}

uint32_t cullInstancesOnCpu(const std::vector<InstanceData>& instances, uint32_t instanceCount,
                            const glm::vec4* frustumPlanes, float boundsRadius, uint32_t* outVisibleIndices)
{
    uint32_t visibleCount = 0;

    for (uint32_t i = 0; i < instanceCount; ++i) {
        const glm::vec3 center = glm::vec3(instances[i].positionScale);
        const float radius = boundsRadius * instances[i].positionScale.w;

        bool visible = true;
        for (int plane = 0; plane < 6; ++plane) {
            const float distance = glm::dot(glm::vec3(frustumPlanes[plane]), center) + frustumPlanes[plane].w;
            if (distance < -radius) {
                visible = false;
                break;
            }
        }

        if (visible) {
            outVisibleIndices[visibleCount] = i;
            ++visibleCount;
        }
    }

    return visibleCount;
}
