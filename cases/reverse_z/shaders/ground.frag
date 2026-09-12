#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) flat in float inLayer;

layout(location = 0) out vec4 outColor;

// 两层地面用同一套棋盘格，只换配色：下层偏冷，上层偏暖。两层形状完全一致，
// 只有固定的高度间距，因此画面上出现任何下层的颜色都说明深度比较选错了表面
const vec3 LOWER_DARK = vec3(0.07, 0.10, 0.18);
const vec3 LOWER_LIGHT = vec3(0.15, 0.21, 0.34);
const vec3 UPPER_DARK = vec3(0.55, 0.20, 0.04);
const vec3 UPPER_LIGHT = vec3(0.90, 0.45, 0.08);

void main()
{
    // 棋盘格的格子尺寸随距离放大，远处格子保持在相近的屏幕尺寸，避免棋盘格自己产生走样
    float viewDistance = max(length(inWorldPosition.xz), 1.0);
    float cellSize = viewDistance * 0.06;
    float checker = mod(floor(inWorldPosition.x / cellSize) + floor(inWorldPosition.z / cellSize), 2.0);

    vec3 color = inLayer < 0.5 ? mix(LOWER_DARK, LOWER_LIGHT, checker)
                               : mix(UPPER_DARK, UPPER_LIGHT, checker);
    outColor = vec4(color, 1.0);
}
