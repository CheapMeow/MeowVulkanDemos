#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 outColor;

void main()
{
    // 参考块直接给出裁剪空间坐标，颜色字段就是线性辐射亮度
    outColor = inColor;
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
}
