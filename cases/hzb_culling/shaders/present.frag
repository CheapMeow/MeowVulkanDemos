#version 450

// 输出：把场景颜色或者金字塔的一层画到交换链上

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;   // x 是否启用遮挡剔除, y 金字塔取向, z 固定层级, w 近平面
    vec4 miscParams;   // x 实例数量, y 是否可视化, z 远平面, w 深度来源
} scene;

layout(set = 0, binding = 2) uniform sampler2D pyramidLevel0;
layout(set = 0, binding = 3) uniform sampler2D colorTexture;
layout(set = 0, binding = 4) uniform sampler2D pyramidLevel1;

// 金字塔里存的是投影之后的非线性深度，换算成视空间距离才能看清两个层级的差别
float viewDistance(float depth)
{
    const float nearPlane = scene.modeParams.w;
    const float farPlane = scene.miscParams.z;
    return (nearPlane * farPlane) / (farPlane - depth * (farPlane - nearPlane));
}

void main()
{
    vec3 result = texture(colorTexture, inUv).rgb;
    if (scene.miscParams.y > 0.5) {
        // 把金字塔的选定层级铺开显示：越近越亮，最远处与背景画成黑色。
        // 两个层级的格子粗细在这张图上直接可比
        const float depth0 = texture(pyramidLevel0, inUv).r;
        const float depth1 = texture(pyramidLevel1, inUv).r;
        const float depth = scene.modeParams.z > 0.5 ? depth1 : depth0;
        const float distance = viewDistance(min(depth, 0.999999));
        result = depth < 1.0 ? vec3(1.0 - clamp(distance / 14.0, 0.0, 1.0)) : vec3(0.0);
    }
    outColor = vec4(result, 1.0);
}
