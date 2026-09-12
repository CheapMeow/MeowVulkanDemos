#pragma once

#include "obj_loader.h"
#include "scene.h"

// 一块从相机脚边一直铺到远裁剪面附近的四边形地面。
// 只有四个顶点，纹理坐标横跨整块地面，远处的深度跨度极大
void buildGroundQuadMesh(float halfWidth, float nearZ, float farZ, MeshData& outMesh);
