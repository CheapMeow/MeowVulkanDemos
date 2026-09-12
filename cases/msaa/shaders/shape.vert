#version 450

layout(location = 0) out vec2 outUv;
layout(location = 1) flat out float outUseAlphaToCoverage;
layout(location = 2) flat out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;  // xy 视口尺寸, zw 保留
    vec4 colorParams;     // rgb 背景亮度, a 保留
    vec4 modeParams;      // x 子场景编号, y 是否用 alpha to coverage, z 片元开销, w 保留
} scene;

layout(set = 0, binding = 1) readonly buffer ShapeBuffer {
    vec4 shapes[];  // xy 左下角, z 边长, w 透明度
} shapeBuffer;

void main()
{
    // 四个顶点拼成一块矩形，角点由顶点编号推出，不需要顶点缓冲
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    vec4 shape = shapeBuffer.shapes[gl_InstanceIndex];

    // 每个形状按实例编号旋转一个角度，边缘因此是斜的，多重采样才会在边界处改变结果
    float angle = float(gl_InstanceIndex) * 0.37;
    float s = sin(angle);
    float c = cos(angle);
    vec2 rotated = vec2(c * corner.x - s * corner.y, s * corner.x + c * corner.y);

    // 每个形状按实例编号取一个深度，深度不同时多重采样才能在相互交叠处按采样点定胜负
    float depth = 0.2 + 0.02 * float(gl_InstanceIndex % 24);
    gl_Position = vec4(shape.xy + rotated * shape.z, depth, 1.0);

    outUv = corner;
    outUseAlphaToCoverage = shape.w < 0.0 ? 1.0 : 0.0;
    outColor = vec3(0.35 + 0.5 * abs(shape.w), 0.30, 0.55 - 0.3 * abs(shape.w));
}
