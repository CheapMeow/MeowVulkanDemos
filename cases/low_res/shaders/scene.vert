#version 450

layout(location = 0) out vec3 outNormal;
layout(location = 1) flat out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 全分辨率尺寸, zw 低分辨率尺寸
    vec4 modeParams;       // x 比例, y 上采样方式, z 深度获取方式, w 双边强度
    vec4 miscParams;       // x 时间, y 光斑强度, zw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer SceneInstanceBuffer {
    vec4 instances[];  // 每块板两个 vec4：rect = (中心 x, y, 半宽, 半高), data = (偏航角, 亮度, 深度, 色相)
} sceneInstances;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// 板绕竖直轴偏航，法线随之偏转，相邻的板因此既有深度台阶也有法线台阶
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
    const vec3 tint = vec3(0.5 + 0.5 * sin(hue * 6.2831853),
                           0.5 + 0.5 * sin(hue * 6.2831853 + 2.094),
                           0.5 + 0.5 * sin(hue * 6.2831853 + 4.188));
    outColor = data.y * tint;
}
