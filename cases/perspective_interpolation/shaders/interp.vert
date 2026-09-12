#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 outUvPerspective;
// 带 noperspective 的输出在屏幕空间线性插值，不做透视除法还原，
// 等价于在片元里用屏幕重心权重对三个顶点的纹理坐标直接加权
layout(location = 1) noperspective out vec2 outUvAffine;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 options;  // x 插值模式, y 图案, z 棋盘格频率, w 保留
} scene;

void main()
{
    outUvPerspective = inUv;
    outUvAffine = inUv;
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
}
