#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;

// 场景通道输出的是纯漫反射的颜色，环境光与直接光在合成通道里按遮蔽量重新组合
void main()
{
    outColor = vec4(inColor, 1.0);
    outNormal = vec4(normalize(inNormal), 1.0);
}
