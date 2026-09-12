#pragma once

#include "obj_loader.h"

#include <vector>

// 参考条上的线性参考值，用来把像素值与理论值对照
const std::vector<float>& referenceBarValues();

// 参考条：一排贴在屏幕左下角的方块，顶点直接给出裁剪空间坐标，
// 值放在纹理坐标的 x 分量里，只有输出编码与色调映射会作用在它上面
void buildReferenceBarMesh(const std::vector<float>& values, MeshData& outMesh);
