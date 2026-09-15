#pragma once

#include "obj_loader.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 球谐阶数上限：五阶共 25 个系数
enum { SH_MAX_BANDS = 5, SH_MAX_COEFFICIENTS = 25 };
// 每个颜色通道的系数打包成多少个 vec4
enum { SH_CHANNEL_VEC4_COUNT = (SH_MAX_COEFFICIENTS + 3) / 4 };
// 三个通道在 uniform 里占的 vec4 总数
enum { SH_TOTAL_VEC4_COUNT = SH_CHANNEL_VEC4_COUNT * 3 };
// 逐像素参考积分的采样数上限（每边 sqrt 个）
enum { SH_REFERENCE_MAX_SAMPLES = 4096 };

// 环境函数的参数，CPU 侧与着色器侧各有一份实现，常数必须一致
struct EnvironmentParams {
    glm::vec3 sunDirection;
    float skyIntensity;
    float glowIntensity;
    float glowExponent;
};

EnvironmentParams defaultEnvironmentParams();

// 解析环境函数：天顶到地平线的渐变、经过一段平滑过渡落到暗地面，再加上一个余弦幂次的光晕。
// 三个部分都连续，没有硬边，因此它的球谐展开在高阶上收敛得很快
glm::vec3 environmentRadiance(const glm::vec3& direction, const EnvironmentParams& params);

// 球面上等距采样做数值积分，把环境投影到球谐系数。
// 输出是 SH_MAX_COEFFICIENTS 个三元组，按 (系数序号 * 3 + 通道) 排列，返回投影用时秒数
double projectEnvironment(const EnvironmentParams& params, float* outCoefficientsRgb);

// 绕 Y 轴旋转环境：只需要旋转系数，每一阶的 ±m 成对混合，m 等于 0 的分量不变
void rotateShAboutY(float* coefficientsRgb, uint32_t bandCount, float angleRadians);

// 漫反射辐照度：E(n) = Σ_l Â_l Σ_m c_l^m Y_l^m(n)，Â_l 是 Ramamoorthi 的卷积因子
glm::vec3 shIrradiance(const float* coefficientsRgb, uint32_t bandCount, const glm::vec3& normal);

// 解析生成的 UV 球，用于承载辐照度
void buildUvSphereMesh(uint32_t segments, float radius, MeshData& outMesh);
