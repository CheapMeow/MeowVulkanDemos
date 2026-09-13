#version 450

// 三条路径共用的形状展开：实例化的板，法线绕竖直轴偏转

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;
layout(location = 2) out vec3 outPosition;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, z 分块尺寸, w 每块的光源上限
    vec4 modeParams;       // x 路径, y 光源数量, z 是否用分块光源列表, w 时间
    vec4 miscParams;       // x 环境光强度, y 保留, zw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer SceneInstanceBuffer {
    vec4 instances[];  // 每块板两个 vec4：rect = (中心 x, y, 半宽, 半高), data = (偏航角, 亮度, 深度, 色相)
} sceneInstances;

void main()
{
    const vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1)) * 2.0 - 1.0;

    const vec4 rect = sceneInstances.instances[gl_InstanceIndex * 2];
    const vec4 data = sceneInstances.instances[gl_InstanceIndex * 2 + 1];

    const float yaw = data.x;
    const float s = sin(yaw);
    const float c = cos(yaw);

    const vec3 local = vec3(corner.x * rect.z, corner.y * rect.w, 0.0);
    const vec3 rotated = vec3(c * local.x, local.y, -s * local.x);
    const vec3 world = vec3(rect.xy, data.z) + rotated;

    gl_Position = scene.viewProjection * vec4(world, 1.0);
    outNormal = vec3(s, 0.0, c);
    outPosition = world;

    const float hue = data.w;
    outColor = data.y * vec3(0.5 + 0.5 * sin(hue * 6.2831853), 0.5 + 0.5 * sin(hue * 6.2831853 + 2.094),
                             0.5 + 0.5 * sin(hue * 6.2831853 + 4.188));
}
