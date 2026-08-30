#include "obj_loader.h"

#include "vk_check.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

static void readWholeFile(const std::string& path, std::vector<char>& outBytes)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        FATAL("打不开文件 %s", path.c_str());
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    outBytes.resize(static_cast<size_t>(size) + 1);
    const size_t readBytes = std::fread(outBytes.data(), 1, static_cast<size_t>(size), file);
    if (readBytes != static_cast<size_t>(size)) {
        FATAL("读取文件 %s 不完整", path.c_str());
    }
    outBytes[static_cast<size_t>(size)] = '\0';
    std::fclose(file);
}

// 解析 "位置/纹理坐标/法线" 形式的面顶点引用，索引从 1 开始，负数表示相对末尾
static void parseFaceVertex(const char*& cursor, int& outPosition, int& outUv, int& outNormal)
{
    outPosition = 0;
    outUv = 0;
    outNormal = 0;

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    char* next = nullptr;
    outPosition = static_cast<int>(std::strtol(cursor, &next, 10));
    if (next == cursor) {
        FATAL("obj 面数据缺少位置索引");
    }
    cursor = next;

    if (*cursor == '/') {
        ++cursor;
        if (*cursor != '/') {
            outUv = static_cast<int>(std::strtol(cursor, &next, 10));
            cursor = next;
        }
        if (*cursor == '/') {
            ++cursor;
            outNormal = static_cast<int>(std::strtol(cursor, &next, 10));
            cursor = next;
        }
    }
}

static int resolveIndex(int rawIndex, size_t count)
{
    if (rawIndex > 0) {
        return rawIndex - 1;
    }
    if (rawIndex < 0) {
        return static_cast<int>(count) + rawIndex;
    }
    FATAL("obj 索引为 0，不符合规范");
}

void loadObj(const std::string& path, MeshData& outMesh)
{
    std::vector<char> bytes;
    readWholeFile(path, bytes);

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    positions.reserve(1 << 16);
    normals.reserve(1 << 16);
    uvs.reserve(1 << 16);

    outMesh.vertices.clear();
    outMesh.indices.clear();
    std::unordered_map<uint64_t, uint32_t> vertexLookup;
    vertexLookup.reserve(1 << 17);

    const char* cursor = bytes.data();
    while (*cursor != '\0') {
        // 定位当前行的起始与结束
        const char* lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\n') {
            ++lineEnd;
        }

        const char* token = cursor;
        while (*token == ' ' || *token == '\t') {
            ++token;
        }

        if (token[0] == 'v' && token[1] == ' ') {
            const char* numbers = token + 2;
            char* next = nullptr;
            glm::vec3 value;
            value.x = std::strtof(numbers, &next);
            numbers = next;
            value.y = std::strtof(numbers, &next);
            numbers = next;
            value.z = std::strtof(numbers, &next);
            positions.push_back(value);
        } else if (token[0] == 'v' && token[1] == 't') {
            const char* numbers = token + 2;
            char* next = nullptr;
            glm::vec2 value;
            value.x = std::strtof(numbers, &next);
            numbers = next;
            value.y = std::strtof(numbers, &next);
            uvs.push_back(value);
        } else if (token[0] == 'v' && token[1] == 'n') {
            const char* numbers = token + 2;
            char* next = nullptr;
            glm::vec3 value;
            value.x = std::strtof(numbers, &next);
            numbers = next;
            value.y = std::strtof(numbers, &next);
            numbers = next;
            value.z = std::strtof(numbers, &next);
            normals.push_back(value);
        } else if (token[0] == 'f' && token[1] == ' ') {
            const char* faceCursor = token + 2;
            uint32_t faceIndices[3] = { 0, 0, 0 };

            for (int corner = 0; corner < 3; ++corner) {
                int rawPosition = 0;
                int rawUv = 0;
                int rawNormal = 0;
                parseFaceVertex(faceCursor, rawPosition, rawUv, rawNormal);

                if (rawUv == 0 || rawNormal == 0) {
                    FATAL("obj 面缺少纹理坐标或法线，当前解析器要求三者齐备");
                }

                const int positionIndex = resolveIndex(rawPosition, positions.size());
                const int uvIndex = resolveIndex(rawUv, uvs.size());
                const int normalIndex = resolveIndex(rawNormal, normals.size());

                const uint64_t key = (static_cast<uint64_t>(positionIndex) << 42) |
                                     (static_cast<uint64_t>(uvIndex) << 21) |
                                     static_cast<uint64_t>(normalIndex);

                std::unordered_map<uint64_t, uint32_t>::iterator found = vertexLookup.find(key);
                if (found != vertexLookup.end()) {
                    faceIndices[corner] = found->second;
                } else {
                    MeshVertex vertex;
                    vertex.position = positions[static_cast<size_t>(positionIndex)];
                    vertex.normal = normals[static_cast<size_t>(normalIndex)];
                    vertex.uv = uvs[static_cast<size_t>(uvIndex)];

                    const uint32_t newIndex = static_cast<uint32_t>(outMesh.vertices.size());
                    outMesh.vertices.push_back(vertex);
                    vertexLookup.insert(std::make_pair(key, newIndex));
                    faceIndices[corner] = newIndex;
                }
            }

            outMesh.indices.push_back(faceIndices[0]);
            outMesh.indices.push_back(faceIndices[1]);
            outMesh.indices.push_back(faceIndices[2]);
        }

        if (*lineEnd == '\0') {
            break;
        }
        cursor = lineEnd + 1;
    }

    if (outMesh.vertices.empty() || outMesh.indices.empty()) {
        FATAL("obj 文件 %s 没有解析出几何数据", path.c_str());
    }

    glm::vec3 minCorner = outMesh.vertices[0].position;
    glm::vec3 maxCorner = outMesh.vertices[0].position;
    for (size_t i = 1; i < outMesh.vertices.size(); ++i) {
        minCorner = glm::min(minCorner, outMesh.vertices[i].position);
        maxCorner = glm::max(maxCorner, outMesh.vertices[i].position);
    }

    // 把模型平移到原点，实例位置即包围球中心，剔除判断因此可以直接用实例位置
    const glm::vec3 center = (minCorner + maxCorner) * 0.5f;
    for (size_t i = 0; i < outMesh.vertices.size(); ++i) {
        outMesh.vertices[i].position -= center;
    }
    outMesh.boundsCenter = glm::vec3(0.0f);

    float maxDistanceSquared = 0.0f;
    for (size_t i = 0; i < outMesh.vertices.size(); ++i) {
        const glm::vec3 offset = outMesh.vertices[i].position;
        const float distanceSquared = glm::dot(offset, offset);
        if (distanceSquared > maxDistanceSquared) {
            maxDistanceSquared = distanceSquared;
        }
    }
    outMesh.boundsRadius = std::sqrt(maxDistanceSquared);

    std::printf("加载 %s: 顶点 %zu, 索引 %zu, 三角形 %zu, 包围球半径 %.4f\n", path.c_str(),
                outMesh.vertices.size(), outMesh.indices.size(), outMesh.indices.size() / 3,
                outMesh.boundsRadius);
}
