// 相机、光源与实例数据的共用声明
#ifndef SHADOW_SCENE_COMMON_GLSL
#define SHADOW_SCENE_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 统一缩放
    vec4 rotation;       // x 绕 Y 轴旋转角度, yzw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量
    vec4 lightColor;      // rgb 颜色, a 强度
    vec4 shadowParams;    // x 基础深度偏移, y 阴影贴图纹素大小, z PCF 半径（纹素）, w 阴影模式
    vec4 shadowOptions;   // x 法线抬升开关, y 角度偏移开关, z 法线抬升距离（世界单位）,
                          // w 光源近平面的归一化修正量, 把归一化深度还原成距离时用
    vec4 shadowPcss;      // x 遮挡物搜索半径（纹素）, y 光源半径换算出的纹素尺度,
                          // z 最小半影（纹素）, w 最大半影（纹素）
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

mat3 rotationAroundY(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return mat3(c, 0.0, -s,
                0.0, 1.0, 0.0,
                s, 0.0, c);
}

// 把世界位置变换到阴影贴图的归一化坐标，inside 表示投影是否落在贴图范围内
vec3 shadowProjection(vec3 worldPosition, out bool inside)
{
    vec4 clip = scene.lightViewProj * vec4(worldPosition, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    inside = uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && projected.z >= 0.0 &&
             projected.z <= 1.0;
    return vec3(uv, projected.z);
}

#endif
