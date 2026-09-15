#version 450

// 方差软阴影：把窗口深度的一阶矩与二阶矩写进两通道颜色附件。
// 主通道用这两项算出区域内深度的均值与方差，再按切比雪夫不等式估算受光比例
layout(location = 0) out vec2 outMoments;

void main()
{
    const float depth = gl_FragCoord.z;
    outMoments = vec2(depth, depth * depth);
}
