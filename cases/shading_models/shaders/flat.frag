#version 450
#extension GL_GOOGLE_include_directive : require

#include "shading_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 outColor;

const vec3 ALBEDO = vec3(0.72, 0.66, 0.55);

void main()
{
    // 面法线由世界位置的屏幕空间导数叉乘得到，一个三角形里的三个像素共用同一个方向
    vec3 faceNormal = normalize(cross(dFdx(inWorldPosition), dFdy(inWorldPosition)));
    // 叉乘的方向由三角形的绕序与视口约定决定，用插值法线把它翻到朝外的一侧
    if (dot(faceNormal, normalize(inWorldNormal)) < 0.0) {
        faceNormal = -faceNormal;
    }

    outColor = vec4(shadeSurface(ALBEDO, faceNormal, inWorldPosition), 1.0);
}
