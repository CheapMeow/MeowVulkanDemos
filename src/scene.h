#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct GLFWwindow;

// 与着色器中的 InstanceData 逐字节对应
struct InstanceData {
    glm::vec4 positionScale;
    glm::vec4 rotation;
};

// 与着色器中的 LightData 逐字节对应
struct LightData {
    glm::vec4 positionRange;
    glm::vec4 color;
};

// 与着色器中的 CameraBuffer 逐字节对应
struct CameraUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 frustumPlanes[6];
    glm::vec4 cullParams;
};

struct Camera {
    glm::vec3 position;
    float yaw;
    float pitch;
    float verticalFieldOfView;
    float nearPlane;
    float farPlane;
    float moveSpeed;
};

// 生成实例网格，并按到网格中心的距离排序。
// 这样界面上减少实例数量时，留下的始终是相机附近的一团，密度保持不变
void buildInstances(uint32_t instanceCount, float spacing, std::vector<InstanceData>& outInstances);
// 光源按固定间距平铺，网格随相机所在区域对齐，保证任何位置都有照明
void updateLights(const glm::vec3& cameraPosition, uint32_t lightCount, float spacing, float range,
                  std::vector<LightData>& lights);

void initCamera(Camera& camera, const std::vector<InstanceData>& instances);
void updateCamera(Camera& camera, GLFWwindow* window, float deltaSeconds);
void fillCameraUniform(const Camera& camera, float aspectRatio, uint32_t instanceCount, float boundsRadius,
                       uint32_t lightCount, CameraUniform& outUniform);

// 逐实例做包围球与视锥的相交判断，可见实例编号写入输出数组
uint32_t cullInstancesOnCpu(const std::vector<InstanceData>& instances, uint32_t instanceCount,
                            const glm::vec4* frustumPlanes, float boundsRadius, uint32_t* outVisibleIndices);
