#pragma once

#include "obj_loader.h"

#include <vector>

// 参考条上的线性辐射亮度，按 2 的整数次幂取，跨越六个数量级
const std::vector<float>& referenceBarValues();

// 参考条：一排贴在屏幕左上角的方块，顶点直接给出裁剪空间坐标，值放在纹理坐标的 x 分量里，
// 只有色调映射与输出编码会作用在它上面
void buildReferenceBarMesh(const std::vector<float>& values, MeshData& outMesh);
