// 相机、地面与实例数据的共用声明
#ifndef REVERSE_Z_SCENE_COMMON_GLSL
#define REVERSE_Z_SCENE_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 统一缩放
    vec4 rotation;       // x 绕 Y 轴旋转角度, y 地面分层编号, zw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量, w 保留
    vec4 groundParams;    // x 上下两层地面的高度间距, y 视图模式, z 深度量化档数, w 保留
} scene;

// 深度可视化：取窗口深度到远平面的距离，按当前深度附件格式的档数量化后开十六次方根。
// 这台相机看向两万单位远，可见范围内的深度值几乎全部挤在远平面附近，直接画出来是一整片饱和色；
// 开方根把靠近远平面的那一小段区间拉开，格式的量化台阶才看得出来。
// 档数为 0 表示浮点格式，不做量化
vec3 depthVisualizationColor(float windowDepth)
{
    float levels = scene.groundParams.z;
    float quantized = levels > 0.5 ? floor(windowDepth * levels + 0.5) / levels : windowDepth;
    float farness = scene.groundParams.w > 0.5 ? quantized : 1.0 - quantized;
    return vec3(pow(clamp(farness, 0.0, 1.0), 1.0 / 16.0));
}

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

#endif
