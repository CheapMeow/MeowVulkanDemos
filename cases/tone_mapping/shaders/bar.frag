#version 450

layout(location = 0) in float inValue;

layout(location = 0) out vec4 outColor;

void main()
{
    // 参考条把已知的线性辐射亮度原样写进高动态范围缓冲，之后与画面走同一条色调映射与输出编码
    outColor = vec4(vec3(inValue), 1.0);
}
