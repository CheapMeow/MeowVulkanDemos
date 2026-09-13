#version 450

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 colorParams;      // rgb 背景亮度, a 保留
    vec4 modeParams;       // x 混合方式, y 是否输出热力图, zw 保留
    vec4 layerParams;      // x 层数, y 节点池容量, zw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer LayerBuffer {
    vec4 layers[];  // 每层两个 vec4：rect = (中心 x, y, 边长, 透明度), color = (rgb, 深度)
} layerBuffer;

void main()
{
    // 四个顶点拼成一块方形色片，角点由顶点编号推出，不需要顶点缓冲
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    vec4 rect = layerBuffer.layers[gl_InstanceIndex * 2];
    vec4 color = layerBuffer.layers[gl_InstanceIndex * 2 + 1];

    vec2 position = rect.xy + (corner - 0.5) * rect.z;
    gl_Position = vec4(position, color.w, 1.0);

    outColor = vec4(color.rgb, rect.w);
}
