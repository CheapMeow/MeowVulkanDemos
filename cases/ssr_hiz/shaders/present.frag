#version 450

// 输出：合成场景颜色与反射，或者显示金字塔层级与步进次数

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SsrUniform {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseProjection;
    vec4 viewportParams;
    vec4 marchParams;
    vec4 modeParams;
    vec4 miscParams;   // x 可视化模式, y 可视化层级, z 屏幕边缘淡出, w 用平均金字塔
    vec4 frameParams;  // x 帧号, y 是否重置累积, z 近平面, w 远平面
} ssr;

layout(set = 0, binding = 2) uniform sampler2D sceneColor;
layout(set = 0, binding = 5) uniform sampler2D pyramidMin;
layout(set = 0, binding = 6) uniform sampler2D pyramidAvg;
layout(set = 0, binding = 8) uniform sampler2D reflectionColor;
layout(set = 0, binding = 9) uniform sampler2D reflectionStep;

float viewDistance(float ndcDepth)
{
    const float nearPlane = ssr.frameParams.z;
    const float farPlane = ssr.frameParams.w;
    return (nearPlane * farPlane) / (farPlane - ndcDepth * (farPlane - nearPlane));
}

float pyramidGray(float depth)
{
    return 1.0 - clamp(viewDistance(min(depth, 0.9999)) / 40.0, 0.0, 1.0);
}

void main()
{
    const vec4 scene = texture(sceneColor, inUv);
    const vec4 reflection = texture(reflectionColor, inUv);
    vec3 result = scene.rgb * (1.0 - reflection.a) + reflection.rgb;

    const int visualization = int(ssr.miscParams.x + 0.5);
    if (visualization == 1) {
        // 并排显示两种金字塔：左半屏取最小值，右半屏取平均值
        const float level = ssr.miscParams.y;
        if (inUv.x < 0.5) {
            const float depth = textureLod(pyramidMin, vec2(inUv.x * 2.0, inUv.y), level).r;
            result = vec3(pyramidGray(depth));
        } else {
            const float depth = textureLod(pyramidAvg, vec2((inUv.x - 0.5) * 2.0, inUv.y), level).r;
            result = vec3(pyramidGray(depth));
        }
    } else if (visualization == 2) {
        // 每像素步进次数除以步数上限的灰度图，越亮表示这一步用的步数越多
        result = vec3(clamp(texture(reflectionStep, inUv).r, 0.0, 1.0));
    }

    outColor = vec4(pow(clamp(result, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
