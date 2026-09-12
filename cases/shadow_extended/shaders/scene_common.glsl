// 相机、光源、级联与实例数据的共用声明
#ifndef SHADOW_EXTENDED_SCENE_COMMON_GLSL
#define SHADOW_EXTENDED_SCENE_COMMON_GLSL

#define MAX_CASCADE_COUNT 4

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 统一缩放
    vec4 rotation;       // x 绕 Y 轴旋转角度, yzw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 lightViewProj[MAX_CASCADE_COUNT];
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量
    vec4 lightColor;      // rgb 颜色, a 强度
    vec4 shadowParams;    // x 基础深度偏移, y 纹素大小, z PCF 半径, w 阴影开关
    vec4 shadowOptions;   // x 法线抬升开关, y 掠射角放大开关, z 抬升距离, w 保留
    vec4 cascadeSplits;   // 各级的远平面距离
    vec4 featureOptions;  // x 级联级数, y 级间融合, z 遮挡物搜索半径, w 半影系数
    vec4 viewOptions;     // x 取值方式, y 坐标计算位置, z 可视化模式, w 摄像机远平面
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

// 每一级一张阴影贴图，索引在分支里写成常量，避免对采样器数组做动态索引
layout(set = 1, binding = 5) uniform sampler2D cascadeMaps[MAX_CASCADE_COUNT];

mat3 rotationAroundY(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return mat3(c, 0.0, -s,
                0.0, 1.0, 0.0,
                s, 0.0, c);
}

// 把世界位置变换到某一级的阴影贴图归一化坐标，inside 表示投影是否落在贴图范围内
vec3 shadowProjectionAt(int cascade, vec3 worldPosition, out bool inside)
{
    mat4 matrix = cascade == 0 ? scene.lightViewProj[0]
                : cascade == 1 ? scene.lightViewProj[1]
                : cascade == 2 ? scene.lightViewProj[2]
                               : scene.lightViewProj[3];
    vec4 clip = matrix * vec4(worldPosition, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    inside = uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && projected.z >= 0.0 &&
             projected.z <= 1.0;
    return vec3(uv, projected.z);
}

// 按视距选级联：落在哪一段就用哪一级
int selectCascade(float viewDepth)
{
    int count = int(scene.featureOptions.x + 0.5);
    for (int i = 0; i < MAX_CASCADE_COUNT - 1; ++i) {
        if (i >= count - 1) {
            break;
        }
        if (viewDepth < scene.cascadeSplits[i]) {
            return i;
        }
    }
    return count - 1;
}

#endif
