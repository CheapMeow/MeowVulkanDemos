#version 450

// 金属度与粗糙度可调的方块阵：实例缓冲给出中心、半边长、颜色与材质参数

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorld;
layout(location = 2) flat out vec3 outColor;
layout(location = 3) flat out vec2 outMaterial;   // x 金属度, y 粗糙度

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 modeParams;       // x 是否使用分离求和, y 环境贴图的层级数, zw 保留
    vec4 miscParams;       // x 时间, yzw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 data[];  // 每个实例三个 vec4：center.xyz + 半边长, color.rgb + 金属度, 粗糙度 + 相位 + 保留
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

    const vec3 tangent = face < 2u ? vec3(1.0, 0.0, 0.0) : (face < 4u ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
    const vec3 bitangent = cross(normal, tangent);
    const float su = (corner & 1u) == 0u ? -1.0 : 1.0;
    const float sv = (corner & 2u) == 0u ? -1.0 : 1.0;

    const vec4 centerSize = instanceBuffer.data[gl_InstanceIndex * 3];
    const vec4 color = instanceBuffer.data[gl_InstanceIndex * 3 + 1];
    const vec4 material = instanceBuffer.data[gl_InstanceIndex * 3 + 2];

    const vec3 world = centerSize.xyz + (normal + tangent * su + bitangent * sv) * centerSize.w;
    gl_Position = scene.viewProjection * vec4(world, 1.0);

    outNormal = normal;
    outWorld = world;
    outColor = color.rgb;
    outMaterial = vec2(color.a, material.x);
}
