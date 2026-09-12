#version 450

layout(location = 0) out vec2 outUv;

// 用三个顶点覆盖整个屏幕，无需顶点缓冲
void main()
{
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outUv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
