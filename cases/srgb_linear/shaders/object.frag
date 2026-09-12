#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量
    vec4 options;         // x 反照率按 sRGB 解释, y 法线按 sRGB 解释, z 输出编码, w 色调映射
} scene;

// 两张贴图的图像是同一份数据，差别只在绑上来的视图按 sRGB 还是按线性解释
layout(set = 0, binding = 1) uniform sampler2D albedoMap;
layout(set = 0, binding = 2) uniform sampler2D normalMap;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

vec3 toneMap(vec3 color, float mode)
{
    if (mode < 0.5) {
        return color;
    }
    if (mode < 1.5) {
        // Reinhard
        return color / (color + vec3(1.0));
    }
    // ACES 的近似拟合
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 encodeOutput(vec3 color, float mode)
{
    if (mode < 0.5) {
        // 线性直写：把线性值原样交给一张线性格式的交换链图像
        return color;
    }
    return pow(color, vec3(1.0 / 2.2));
}

// 用屏幕空间导数构造切线空间，无需模型提供切线数据
vec3 sampleWorldNormal()
{
    vec3 tangentNormal = texture(normalMap, inUv).xyz * 2.0 - 1.0;

    vec3 dPositionX = dFdx(inWorldPosition);
    vec3 dPositionY = dFdy(inWorldPosition);
    vec2 dUvX = dFdx(inUv);
    vec2 dUvY = dFdy(inUv);

    vec3 normal = normalize(inWorldNormal);
    vec3 tangent = normalize(dPositionX * dUvY.t - dPositionY * dUvX.t);
    vec3 bitangent = -normalize(cross(normal, tangent));
    mat3 tangentToWorld = mat3(tangent, bitangent, normal);

    return normalize(tangentToWorld * tangentNormal);
}

void main()
{
    vec3 albedo = texture(albedoMap, inUv).rgb;
    vec3 normal = sampleWorldNormal();

    vec3 lightDirection = normalize(scene.lightDirection.xyz);
    vec3 viewDirection = normalize(scene.cameraPosition.xyz - inWorldPosition);
    vec3 halfway = normalize(viewDirection + lightDirection);

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotH = max(dot(normal, halfway), 0.0);
    float specular = pow(nDotH, 48.0);

    // 一点很弱的环境项，让背光面不是全黑
    vec3 color = albedo * (vec3(0.05) + vec3(0.95) * nDotL) + vec3(specular) * 0.35;

    color = toneMap(color, scene.options.w);
    outColor = vec4(encodeOutput(color, scene.options.z), 1.0);
}
