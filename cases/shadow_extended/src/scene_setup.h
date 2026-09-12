#pragma once

#include "obj_loader.h"
#include "scene.h"

#include <cstdint>
#include <vector>

// 把实例按网格摆放在 y=0 的地面上，并按到网格中心的距离排序，
// 这样界面上减少实例数量时，留下的始终是中心附近的一团。
// objectCenterHeight 是缩放前底面落地需要的中心高度，函数内部按 scale 放大
void buildShadowInstances(uint32_t instanceCount, float spacing, float objectCenterHeight, float scale,
                          std::vector<InstanceData>& outInstances);

// 网格布局在水平方向上的半边长，用来拟合光源的正交投影
float shadowGridHalfExtent(uint32_t instanceCount, float spacing);

// 模型底面刚好落在 y=0 时，物体中心应当抬高到的高度
float objectCenterHeightForMesh(const MeshData& mesh);

// 一个边长为 1、中心在原点、法线朝上的水平面
void buildGroundPlaneMesh(MeshData& outMesh);
