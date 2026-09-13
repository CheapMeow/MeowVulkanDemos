#version 450

layout(location = 0) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
    // 细杆与背景都是纯色，对比度完全由两者的亮度差给出
    outColor = vec4(inColor, 1.0);
}
