#pragma once

#include "obj_loader.h"

#include <cstdint>
#include <vector>

// 方向反照率表的尺寸：第一个下标是粗糙度，第二个是法线与视线夹角的余弦
enum { BRDF_TABLE_ROUGHNESS_COUNT = 64, BRDF_TABLE_COSINE_COUNT = 32 };
// 多次散射的平均反照率表长度，与粗糙度方向一致
enum { BRDF_AVERAGE_COUNT = BRDF_TABLE_ROUGHNESS_COUNT };

// 法线分布的形状，建表时必须与着色器用同一个
enum BrdfDistribution {
    BRDF_DISTRIBUTION_GGX = 0,
    BRDF_DISTRIBUTION_BECKMANN = 1,
    BRDF_DISTRIBUTION_BLINN_PHONG = 2,
};

struct BrdfTableResult {
    // BRDF_TABLE_ROUGHNESS_COUNT * BRDF_TABLE_COSINE_COUNT 个方向反照率，按粗糙度优先排列
    std::vector<float> directionalAlbedo;
    // 每个粗糙度上的平均反照率
    std::vector<float> averageAlbedo;
    // 建表用时，单位秒
    double seconds;
};

// 在 CPU 上用 GGX 重要性采样对同一个微表面 BRDF 做蒙特卡洛积分，得到方向反照率
// E(μ) = ∫ f(μ, μ_o) μ_o dμ_o dφ 与它的余弦加权平均 E_avg = 2 ∫ E(μ) μ dμ
BrdfTableResult buildBrdfTables(uint32_t distribution, float reflectance, uint32_t sampleCount);

// 解析生成的 UV 球，用于承载 BRDF 着色
void buildUvSphereMesh(uint32_t segments, float radius, MeshData& outMesh);
