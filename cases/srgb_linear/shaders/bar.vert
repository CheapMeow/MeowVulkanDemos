#version 450

// 参考条的顶点直接给出裁剪空间坐标，值放在 uv.x 里
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;

layout(location = 0) out float outValue;

void main()
{
    outValue = inUv.x;
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
}
