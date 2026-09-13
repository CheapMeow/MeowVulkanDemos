#version 450

layout(location = 0) out vec2 outUv;
layout(location = 1) flat out float outAlpha;
layout(location = 2) flat out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 colorParams;      // rgb 背景亮度, a 保留
    vec4 modeParams;       // x 处理方式, y alpha 阈值, zw 保留
    vec4 animationParams;  // x 累计旋转角度, yzw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer ShapeBuffer {
    vec4 shapes[];  // 每块形状两个 vec4：rect = (左下角 x, y, 边长, 基准透明度), params = (深度, 相位, 种子, 保留)
} shapeBuffer;

// 由种子生成的伪随机数，用来给每块形状一点颜色差异
float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

void main()
{
    // 四个顶点拼成一块矩形，角点由顶点编号推出，不需要顶点缓冲
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    vec4 rect = shapeBuffer.shapes[gl_InstanceIndex * 2];
    vec4 params = shapeBuffer.shapes[gl_InstanceIndex * 2 + 1];

    // 植被围着自己的中心缓慢旋转，相位与累计角度一起决定当前角度
    float angle = params.y + scene.animationParams.x;
    float s = sin(angle);
    float c = cos(angle);
    vec2 rotated = vec2(c * corner.x - s * corner.y, s * corner.x + c * corner.y);

    vec2 center = rect.xy + rect.z * 0.5;
    // 逻辑深度映射到裁剪空间的 z，深度越大越远。不透明方式按由近到远绘制时，
    // 前面的卡片才能把后面的片元挡在深度测试之外
    float ndcDepth = clamp(0.5 + 0.45 * params.x, 0.02, 0.98);
    gl_Position = vec4(center + rotated * rect.z, ndcDepth, 1.0);

    outUv = corner;
    outAlpha = rect.w;
    outColor = mix(vec3(0.16, 0.36, 0.14), vec3(0.30, 0.52, 0.18), hash11(params.z * 9.17 + 1.0));
}
