#pragma once

#include "obj_loader.h"
#include "scene.h"

#include <cstdint>
#include <vector>

// 上下两层地面共用一张网格，网格沿视线方向分段，避免单个三角形跨度过大
enum { GROUND_SEGMENT_COUNT = 96 };

// 网格的起止距离，以及半宽与距离的比例。比例取得比视锥的横向张开还大一点，
// 任何距离上地面都盖满整个可见范围
constexpr float GROUND_START_DISTANCE = 0.5f;
constexpr float GROUND_END_DISTANCE = 20000.0f;
constexpr float GROUND_HALF_WIDTH_RATIO = 1.5f;

// 物体在地面上的摆放：第一排的距离、每排的距离比例、横向偏移与距离的比例、
// 缩放与距离的比例。缩放跟着距离放大，物体的屏幕尺寸保持接近，远处物体也看得见
constexpr float OBJECT_FIRST_DISTANCE = 24.0f;
constexpr float OBJECT_DISTANCE_RATIO = 1.45f;
constexpr float OBJECT_LATERAL_RATIO = 0.22f;
constexpr float OBJECT_SCALE_RATIO = 0.11f;

// 位于 y = 0 的水平条带，法线朝上，中心在原点。两层地面共用它
void buildGroundStripMesh(MeshData& outMesh);

// 模型底面刚好落在 y=0 时，物体中心应当抬高到的高度
float objectCenterHeightForMesh(const MeshData& mesh);

// 地面上摆放的物体实例，每排两个，距离按固定比例递增
void buildObjectInstances(uint32_t instanceCount, float objectCenterHeight,
                          std::vector<InstanceData>& outInstances);

// 两层地面的实例变换：位置与缩放相同，rotation.y 是分层编号，由顶点着色器换成高度
void buildGroundInstances(std::vector<InstanceData>& outInstances);
