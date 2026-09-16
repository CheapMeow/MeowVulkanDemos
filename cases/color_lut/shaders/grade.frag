#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GradeBuffer {
    vec4 params;   // x 处理方式, y 三维表格尺寸, z 曝光倍数, w 保留
} grade;

layout(set = 0, binding = 1) uniform sampler2D sceneColor;
// 三维查找表摊平成宽 size 乘 size、高 size 的二维条带，每块是蓝通道的一个切片
layout(set = 0, binding = 2) uniform sampler2D lutTexture;
// 每通道一条 256 项的一维曲线
layout(set = 0, binding = 3) uniform sampler2D curveTexture;

const float MODE_NONE = 0.0;
const float MODE_LUT3D = 1.0;
const float MODE_CURVES = 2.0;
const float MODE_ANALYTIC = 3.0;

// 与 CPU 侧同名函数保持一致
vec3 gradeReference(vec3 color)
{
    const vec3 clamped = clamp(color, 0.0, 1.0);

    vec3 s = clamped * clamped * (3.0 - 2.0 * clamped);
    s = mix(clamped, s, 0.6);

    const float luminance = dot(s, vec3(0.2126, 0.7152, 0.0722));
    const vec3 warm = vec3(1.05, 1.0, 0.92);
    const vec3 cool = vec3(0.92, 0.97, 1.10);
    s *= mix(cool, warm, smoothstep(0.2, 0.8, luminance));

    const float saturation = mix(0.55, 1.10, smoothstep(0.05, 0.45, luminance));
    const float grey = dot(s, vec3(0.2126, 0.7152, 0.0722));
    s = mix(vec3(grey), s, saturation);

    return clamp(s, 0.0, 1.0);
}

vec3 toneMapAndEncode(vec3 color)
{
    color *= grade.params.z;
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    const vec3 mapped = clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
    const vec3 low = mapped * 12.92;
    const vec3 high = 1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), mapped));
}

// 蓝通道决定落在哪一块切片上，红绿通道在块内取位置，两块之间按蓝通道的小数部分混合
vec3 sampleThreeDimensionalLut(vec3 color)
{
    const float size = grade.params.y;
    const float blue = clamp(color.b, 0.0, 1.0) * (size - 1.0);
    const float sliceLow = floor(blue);
    const float sliceHigh = min(sliceLow + 1.0, size - 1.0);
    const float fraction = blue - sliceLow;

    const float x = clamp(color.r, 0.0, 1.0) * (size - 1.0) + 0.5;
    const float y = clamp(color.g, 0.0, 1.0) * (size - 1.0) + 0.5;

    const vec2 uvLow = vec2((sliceLow * size + x) / (size * size), y / size);
    const vec2 uvHigh = vec2((sliceHigh * size + x) / (size * size), y / size);
    return mix(texture(lutTexture, uvLow).rgb, texture(lutTexture, uvHigh).rgb, fraction);
}

// 一维曲线：三个通道各自查自己那条曲线，查表之前先把这一路的输入换成该通道的值
vec3 sampleOneDimensionalLut(vec3 color)
{
    const float r = texture(curveTexture, vec2(color.r, 0.5)).r;
    const float g = texture(curveTexture, vec2(color.g, 0.5)).g;
    const float b = texture(curveTexture, vec2(color.b, 0.5)).b;
    return vec3(r, g, b);
}

void main()
{
    const vec3 sceneValue = texture(sceneColor, inUv).rgb;
    const vec3 displayed = toneMapAndEncode(sceneValue);

    vec3 graded = displayed;
    if (grade.params.x > MODE_CURVES - 0.5) {
        if (grade.params.x > MODE_ANALYTIC - 0.5) {
            graded = gradeReference(displayed);
        } else {
            graded = sampleOneDimensionalLut(displayed);
        }
    } else if (grade.params.x > MODE_LUT3D - 0.5) {
        graded = sampleThreeDimensionalLut(displayed);
    }

    outColor = vec4(graded, 1.0);
}
