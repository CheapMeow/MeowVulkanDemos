#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec2 inUv;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 options;  // x 视图模式, y 保留, z 保留, w 保留
} scene;

layout(set = 0, binding = 1) uniform sampler2D detailMap;

layout(location = 0) out vec4 outColor;

// 视图模式
const float VIEW_NORMAL = 0.0;
const float VIEW_LOD = 1.0;
const float VIEW_FOOTPRINT = 2.0;

// 着色器按导数算出的层级：rho 取两个方向导数模长的较大值，层级取 log2(rho)
float shaderLod(vec2 uv, out float footprint)
{
    vec2 textureSize = vec2(textureSize(detailMap, 0));
    vec2 uvPerPixelX = dFdx(uv) * textureSize;
    vec2 uvPerPixelY = dFdy(uv) * textureSize;
    float lengthX = length(uvPerPixelX);
    float lengthY = length(uvPerPixelY);
    footprint = max(lengthX, lengthY);
    return log2(max(footprint, 1e-6));
}

void main()
{
    float mode = scene.options.x;
    float footprint = 0.0;
    float computedLod = shaderLod(inUv, footprint);
    float hardwareLod = textureQueryLod(detailMap, inUv).x;
    float mipCount = float(textureQueryLevels(detailMap));

    if (mode > 1.5) {
        // 采样足迹：两个方向的导数换算成纹素长度后的比值，比值越大说明越斜视
        vec2 textureSize = vec2(textureSize(detailMap, 0));
        float lengthX = length(dFdx(inUv) * textureSize);
        float lengthY = length(dFdy(inUv) * textureSize);
        float major = max(lengthX, lengthY);
        float minor = min(lengthX, lengthY);
        float ratio = major / max(minor, 1e-6);
        float gray = clamp(log2(max(major, 1e-6)) / mipCount, 0.0, 1.0);
        outColor = vec4(gray, clamp((ratio - 1.0) / 7.0, 0.0, 1.0), 0.0, 1.0);
        return;
    }

    if (mode > 0.5) {
        // 层级对比：左半屏是着色器按导数算出的层级，右半屏是硬件 textureQueryLod 给出的层级
        float lod = gl_FragCoord.x < 0.5 * scene.options.z ? computedLod : hardwareLod;
        float gray = clamp((lod + 2.0) / (mipCount + 2.0), 0.0, 1.0);
        outColor = vec4(vec3(gray), 1.0);
        return;
    }

    outColor = vec4(texture(detailMap, inUv).rgb, 1.0);
}
