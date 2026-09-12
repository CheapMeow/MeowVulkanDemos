#version 450

// 矩模式：把窗口深度的一阶与二阶矩写进两通道颜色附件
layout(location = 0) out vec2 outMoments;

void main()
{
    float depth = gl_FragCoord.z;
    outMoments = vec2(depth, depth * depth);
}
