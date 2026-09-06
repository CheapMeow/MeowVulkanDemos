#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    // 局部空间包围球，供视锥剔除使用
    glm::vec3 boundsCenter;
    float boundsRadius;
};

// 解析仅含三角面、单一材质的 obj，输入是随包资源的字节，由调用方用 readAssetBytes 读入
void loadObjFromMemory(const std::vector<unsigned char>& fileBytes, MeshData& outMesh);
