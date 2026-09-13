#version 450

// 立方体展开：实例缓冲给出中心、颜色与尺寸，可见面朝外

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec4 outColorReflectivity;   // rgb 颜色, a 反射强度
layout(location = 3) out float outRoughness;

layout(set = 0, binding = 0) uniform SsrUniform {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseProjection;
    vec4 viewportParams;   // xy 视口尺寸, z 金字塔层级数, w 是否启用反射
    vec4 marchParams;      // x 步数上限, y 步长, z 厚度阈值, w 抖动强度
    vec4 modeParams;       // x 步进模式, y 二分细化, z 累积混合系数, w 抖动开关
    vec4 miscParams;       // x 可视化模式, y 可视化层级, z 屏幕边缘淡出, w 用平均金字塔
    vec4 frameParams;      // x 帧号, y 是否重置累积, zw 保留
} ssr;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 instances[];   // 每个实例三个 vec4：center.xyz, color.rgb + 反射强度, size.xyz + 粗糙度
} instanceBuffer;

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

    const vec3 tangent =
        face < 2u ? vec3(1.0, 0.0, 0.0) : (face < 4u ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
    const vec3 bitangent = cross(normal, tangent);
    const float su = (corner & 1u) == 0u ? -1.0 : 1.0;
    const float sv = (corner & 2u) == 0u ? -1.0 : 1.0;

    const vec4 center = instanceBuffer.instances[gl_InstanceIndex * 3];
    const vec4 colorReflectivity = instanceBuffer.instances[gl_InstanceIndex * 3 + 1];
    const vec4 sizeRoughness = instanceBuffer.instances[gl_InstanceIndex * 3 + 2];

    const vec3 world = center.xyz + (normal + tangent * su + bitangent * sv) * sizeRoughness.xyz;
    gl_Position = ssr.viewProjection * vec4(world, 1.0);

    outWorldPosition = world;
    outWorldNormal = normal;
    outColorReflectivity = colorReflectivity;
    outRoughness = sizeRoughness.w;
}
