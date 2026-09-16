#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 一排小亮球加一块地面共用的顶点格式
struct SceneVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
    // 大于零表示自发光，用于产生散景光斑
    float emission;
};

// 地面加一排按深度排布的小亮球，另有两个大球用来观察前景与背景的模糊
void buildScene(std::vector<SceneVertex>& outVertices, std::vector<uint32_t>& outIndices);

// 薄透镜模型的弥散圆直径，单位是像素。focalLength 与 focusDistance 用世界单位，
// sensorWidth 是世界单位下成像面的宽度，imageWidth 是画面的宽度（像素）
float circleOfConfusionPixels(float viewDistance, float focalLength, float fNumber,
                             float focusDistance, float sensorWidth, float imageWidth);
