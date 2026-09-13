#version 450

// 立方体的展开：实例缓冲给出中心与尺寸。默认从可见列表取实例编号，
// 深度预通道里则直接用实例编号绘制全部实例

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

// 深度预通道直接用实例编号绘制全部实例，场景通道从可见列表取实例编号
layout(constant_id = 0) const bool DIRECT_INSTANCE = false;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, z 包围盒扩大系数, w 层级模式（0 固定层级, 1 按屏幕尺寸）
    vec4 modeParams;       // x 是否启用遮挡剔除, y 金字塔取向, z 固定层级, w 近平面
    vec4 miscParams;       // x 实例数量, y 是否可视化, z 远平面, w 深度来源（0 上一帧, 1 当前帧）
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 instances[];  // 每个实例两个 vec4：center.xyz + 半边长, color.rgb + 保留
} instanceBuffer;

layout(set = 0, binding = 5) readonly buffer VisibleBuffer {
    uint visible[];
} visibleBuffer;

void main()
{
    const uint vertexIndex = gl_VertexIndex % 24u;
    const uint face = vertexIndex / 4u;
    const uint corner = vertexIndex % 4u;

    vec3 normal;
    if (face == 0u) {
        normal = vec3(0.0, 0.0, 1.0);
    } else if (face == 1u) {
        normal = vec3(0.0, 0.0, -1.0);
    } else if (face == 2u) {
        normal = vec3(1.0, 0.0, 0.0);
    } else if (face == 3u) {
        normal = vec3(-1.0, 0.0, 0.0);
    } else if (face == 4u) {
        normal = vec3(0.0, 1.0, 0.0);
    } else {
        normal = vec3(0.0, -1.0, 0.0);
    }

    const vec3 tangent = face < 2u ? vec3(1.0, 0.0, 0.0) : (face < 4u ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
    const vec3 bitangent = cross(normal, tangent);
    const float su = (corner & 1u) == 0u ? -1.0 : 1.0;
    const float sv = (corner & 2u) == 0u ? -1.0 : 1.0;

    // 可见列表里存的是实例编号，被剔除的实例不会出现在这里
    const uint instanceIndex =
        DIRECT_INSTANCE ? uint(gl_InstanceIndex) : visibleBuffer.visible[gl_InstanceIndex];
    const vec4 centerSize = instanceBuffer.instances[instanceIndex * 2];
    const vec4 color = instanceBuffer.instances[instanceIndex * 2 + 1];

    const vec3 world = centerSize.xyz + (normal + tangent * su + bitangent * sv) * centerSize.w;
    gl_Position = scene.viewProjection * vec4(world, 1.0);

    outNormal = normal;
    outColor = color.rgb;
}
