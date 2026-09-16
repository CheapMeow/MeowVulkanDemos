#version 450

// 全屏三角形：不需要顶点缓冲，gl_VertexIndex 直接决定三个裁剪空间顶点
void main()
{
    vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
