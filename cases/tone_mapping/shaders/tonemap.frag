#version 450

layout(location = 0) in vec2 inUv;

layout(set = 0, binding = 0) uniform ToneMapBuffer {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 sunDirection;
    vec4 skyParams;
    vec4 operatorParams;   // x 算子, y 作用方式, z 曝光倍数, w 扩展 Reinhard 的白点
    vec4 encodingParams;   // x 输出编码, yzw 保留
} tm;

layout(set = 0, binding = 1) uniform sampler2D sceneColor;

layout(location = 0) out vec4 outColor;

const float OP_NONE = 0.0;
const float OP_REINHARD = 1.0;
const float OP_REINHARD_EXTENDED = 2.0;
const float OP_ACES = 3.0;
const float OP_FILMIC = 4.0;

const float MODE_PER_CHANNEL = 0.0;

const float ENCODING_GAMMA = 1.0;
const float ENCODING_SRGB = 2.0;

// Hable 的经验曲线，下面用各自的常数把它做成不同的算子
float filmicCurve(float x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// 单分量的曲线，输入是已经乘过曝光的线性亮度，输出落在 [0,1]
float toneCurve(float x, float op, float whitePoint)
{
    if (op < OP_REINHARD + 0.5) {
        return x / (1.0 + x);
    }
    if (op < OP_REINHARD_EXTENDED + 0.5) {
        return x * (1.0 + x / (whitePoint * whitePoint)) / (1.0 + x);
    }
    if (op < OP_ACES + 0.5) {
        // Narkowicz 对 ACES 的解析拟合
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    }
    if (op < OP_FILMIC + 0.5) {
        const float WHITE = 11.2;
        return clamp(filmicCurve(x) / filmicCurve(WHITE), 0.0, 1.0);
    }
    return x;
}

vec3 toneMap(vec3 color, float op, float mode, float whitePoint)
{
    if (op < OP_NONE + 0.5) {
        return clamp(color, 0.0, 1.0);
    }
    if (mode < MODE_PER_CHANNEL + 0.5) {
        // 逐通道：三个分量各自过曲线，压缩率只由自己的数值决定，亮且饱和的颜色会向白色靠
        return vec3(toneCurve(color.r, op, whitePoint), toneCurve(color.g, op, whitePoint),
                    toneCurve(color.b, op, whitePoint));
    }
    // 按亮度：只压缩亮度，三个分量的比例原样保留
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float mapped = toneCurve(luma, op, whitePoint);
    return color * (mapped / max(luma, 1e-6));
}

vec3 encodeOutput(vec3 color, float mode)
{
    color = clamp(color, 0.0, 1.0);
    if (mode > ENCODING_SRGB - 0.5) {
        // sRGB 传递函数的精确分段形式
        const vec3 low = color * 12.92;
        const vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
        return mix(low, high, step(vec3(0.0031308), color));
    }
    if (mode > ENCODING_GAMMA - 0.5) {
        return pow(color, vec3(1.0 / 2.2));
    }
    return color;
}

void main()
{
    // 曝光在色调映射之前作用在线性辐射亮度上
    vec3 color = texture(sceneColor, inUv).rgb * tm.operatorParams.z;
    color = toneMap(color, tm.operatorParams.x, tm.operatorParams.y, tm.operatorParams.w);
    outColor = vec4(encodeOutput(color, tm.encodingParams.x), 1.0);
}
