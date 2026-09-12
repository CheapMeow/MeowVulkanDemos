#version 450

// 深度模式：把片元的窗口深度写进颜色附件
layout(location = 0) out float outDepth;

void main()
{
    outDepth = gl_FragCoord.z;
}
