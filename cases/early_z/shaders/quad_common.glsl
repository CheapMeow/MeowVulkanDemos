// 片元开销、实例数据与顺序数据的共用声明
#ifndef EARLY_Z_QUAD_COMMON_GLSL
#define EARLY_Z_QUAD_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 半边长
    vec4 color;          // rgb 颜色, a alpha 阈值
    vec4 flags;          // x 是否走 alpha test, yzw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 frameParams;  // x 片元开销的循环次数, y 是否插入永不成立的 discard, z 是否走 alpha test, w 保留
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

layout(set = 0, binding = 2) readonly buffer OrderBuffer {
    uint indices[];
} orderBuffer;

// 程序生成的镂空掩码，不引入贴图资源
float coverageMask(vec2 uv)
{
    float a = sin(uv.x * 37.0) * sin(uv.y * 41.0);
    float b = sin(uv.x * 13.0 + 1.7) * sin(uv.y * 17.0 + 0.9);
    return step(0.0, a + 0.6 * b);
}

// 片元开销：按循环次数做一串非线性运算，结果只用于染色，不会被优化掉
vec3 expensiveShade(vec2 uv, vec3 baseColor, float iterations)
{
    float value = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (float(i) >= iterations) {
            break;
        }
        float t = float(i) * 0.017 + uv.x * 3.1;
        value += sin(t) * cos(t * 1.3 + uv.y * 2.7) * 0.5 + 0.5;
    }
    value = fract(value * 0.01);
    return baseColor * (0.85 + 0.3 * value);
}

// alpha test 与永不成立的 discard 都会打断 early-Z，这里按开关插入。
// 两个条件都取自 uniform 与插值量，编译期无法判定，着色器里始终存在 discard 语句。
// discard 只在片元阶段可用，这一段按阶段条件编译
#ifdef GL_FRAGMENT_SHADER
void applyDiscard(vec2 uv, float alphaTested, float threshold)
{
    if (scene.frameParams.y > 0.5) {
        // 纹理坐标落在 0 到 1 之间，这个条件永远不成立
        if (uv.x * uv.y < -1000000.0) {
            discard;
        }
    }
    if (scene.frameParams.z > 0.5 && alphaTested > 0.5) {
        if (coverageMask(uv) < threshold) {
            discard;
        }
    }
}
#endif

#endif
