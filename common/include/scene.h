#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 相机操作在这一层只认按键状态，窗口库负责把它填满，安卓端没有键盘就全填 false
struct CameraInput {
    bool turnLeft;
    bool turnRight;
    bool turnUp;
    bool turnDown;
    bool moveForward;
    bool moveBack;
    bool moveLeft;
    bool moveRight;
    bool moveUp;
    bool moveDown;
    bool fast;
};

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

// 相机矩阵。相机相关的字段在任何 case 里都一样，cullParams 之类的用法差异由 case 自己扩展
struct CameraMatrices {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
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
void updateCamera(Camera& camera, const CameraInput& input, float deltaSeconds);

// 相机的视图矩阵、投影矩阵、二者乘积与世界位置。投影矩阵已按 Vulkan 的裁剪空间翻转 Y 轴
void fillCameraMatrices(const Camera& camera, float aspectRatio, CameraMatrices& outMatrices);

// 相机朝向的单位向量
glm::vec3 cameraForward(const Camera& camera);

// 从视图投影矩阵按 Gribb-Hartmann 方法取出六个视锥平面，平面指向视锥内部为正
void extractFrustumPlanes(const glm::mat4& viewProjection, glm::vec4* outPlanes);

// 逐实例做包围球与视锥的相交判断，可见实例编号写入输出数组
uint32_t cullInstancesOnCpu(const std::vector<InstanceData>& instances, uint32_t instanceCount,
                            const glm::vec4* frustumPlanes, float boundsRadius, uint32_t* outVisibleIndices);
