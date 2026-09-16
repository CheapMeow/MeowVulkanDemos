#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 图案纹理的尺寸
enum { PATTERN_TEXTURE_SIZE = 512 };

// 解析图案：一段屏幕中心为原点的频率渐变（zone plate）加上三条细条纹。
// 频率沿半径线性增长，用于观察欠采样时的摩尔纹与各种重建核的表现
float patternValue(const glm::vec2& uv);

// 解析图案的彩色版本，三个通道用不同的频率与相位
glm::vec3 patternColor(const glm::vec2& uv);

// 把解析图案采样成一张纹理，返回值是 RGBA8 的像素
std::vector<unsigned char> buildPatternPixels();
