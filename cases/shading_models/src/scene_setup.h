#pragma once

#include "obj_loader.h"

#include <cstdint>

// 解析生成的 UV 球。经线方向的段数决定三角形密度，纬度方向的环数取段数的一半，
// 顶点法线用解析法线，因此球面是光滑的，只有轮廓会随段数变化
void buildUvSphereMesh(uint32_t segments, float radius, MeshData& outMesh);
