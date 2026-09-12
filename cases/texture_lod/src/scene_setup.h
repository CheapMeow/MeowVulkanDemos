#pragma once

#include "obj_loader.h"
#include "scene.h"

// 一块位于水平面上的方形地面，纹理坐标横跨整块地面
void buildGroundQuadMesh(float halfExtent, MeshData& outMesh);

// 程序生成的高频细节贴图：棋盘格叠加细噪声，写入 RGBA8 像素。
// 棋盘格给出清晰的层级变化，噪声让缩小后的平均颜色有内容
void buildDetailTexturePixels(uint32_t size, std::vector<unsigned char>& outPixels);
