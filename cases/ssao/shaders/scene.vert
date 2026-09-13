#version 450

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 modeParams;       // x 遮蔽量乘到哪一档光照, y 采样数, z 采样半径, w 核大小
    vec4 miscParams;       // x 法线加权开关, y 遮蔽强度, z 近平面, w 远平面
} scene;

layout(set = 0, binding = 1) readonly buffer SceneInstanceBuffer {
    vec4 instances[];  // 每块板两个 vec4：rect = (中心 x, y, 半宽, 半高), data = (偏航角, 亮度, 深度, 色相)
} sceneInstances;

// 板绕竖直轴偏航，相邻的板之间既有深度台阶也有法线台阶，正是屏幕空间遮蔽要处理的情形
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

    const float hue = data.w;
    outColor = data.y * vec3(0.5 + 0.5 * sin(hue * 6.2831853), 0.5 + 0.5 * sin(hue * 6.2831853 + 2.094),
                             0.5 + 0.5 * sin(hue * 6.2831853 + 4.188));
}
