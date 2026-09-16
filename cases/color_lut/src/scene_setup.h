#pragma once

#include "obj_loader.h"

#include <cstdint>
#include <vector>

// 调色查找表的格子数，三维表摊平成 X 乘 Y 的二维条带，一维表只有一行
enum { GRADE_LUT_MIN_SIZE = 8, GRADE_LUT_MAX_SIZE = 64, GRADE_ONE_DIMENSIONAL_SIZE = 256 };

// 一排彩色球加一排参考块共用的顶点格式
struct ColorVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

// 一排彩色球，颜色用来检验调色对色相的影响
void buildSphereRow(std::vector<ColorVertex>& outVertices, std::vector<uint32_t>& outIndices);

// 屏幕底部两条参考块：左边是十六级灰阶，右边是八块饱和色。
// 颜色字段直接写线性辐射亮度，绘制时不参与光照
void buildReferencePatches(std::vector<ColorVertex>& outVertices, std::vector<uint32_t>& outIndices);

// 调色的解析变换，CPU 与着色器各有一份实现，常数必须一致
glm::vec3 gradeReference(const glm::vec3& color);

// 三维查找表：摊平成 宽度 size 乘 size、高度 size 的二维条带，每块是蓝通道的一个切片
std::vector<unsigned char> buildThreeDimensionalLut(uint32_t size);

// 一维查找表：每通道一条 256 项的曲线，取自解析变换对中性灰的响应
std::vector<unsigned char> buildOneDimensionalLut();
