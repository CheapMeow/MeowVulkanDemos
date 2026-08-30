#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec2 inUv;

layout(set = 1, binding = 0) uniform sampler2D gbufferAlbedoOcclusion;
layout(set = 1, binding = 1) uniform sampler2D gbufferNormalRoughness;
layout(set = 1, binding = 2) uniform sampler2D gbufferPositionMetallic;

struct LightData {
    vec4 positionRange;  // xyz 世界位置, w 影响半径
    vec4 color;          // rgb 颜色, a 强度
};

layout(set = 1, binding = 3) readonly buffer LightBuffer {
    LightData lights[];
} lightBuffer;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// 法线分布函数：Trowbridge-Reitz GGX
float distributionGGX(vec3 normal, vec3 halfway, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(normal, halfway), 0.0);
    float nDotH2 = nDotH * nDotH;

    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    denominator = PI * denominator * denominator;

    return a2 / denominator;
}

float geometrySchlickGGX(float nDotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / (nDotV * (1.0 - k) + k);
}

float geometrySmith(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness)
{
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    vec4 albedoOcclusion = texture(gbufferAlbedoOcclusion, inUv);
    vec4 normalRoughness = texture(gbufferNormalRoughness, inUv);
    vec4 positionMetallic = texture(gbufferPositionMetallic, inUv);

    // 没有几何写入的像素保持背景色
    if (dot(normalRoughness.xyz, normalRoughness.xyz) < 0.01) {
        outColor = vec4(0.02, 0.025, 0.035, 1.0);
        return;
    }

    vec3 albedo = pow(albedoOcclusion.rgb, vec3(2.2));
    float ambientOcclusion = albedoOcclusion.a;
    vec3 normal = normalize(normalRoughness.xyz);
    float roughness = clamp(normalRoughness.a, 0.04, 1.0);
    vec3 worldPosition = positionMetallic.xyz;
    float metallic = positionMetallic.a;

    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    vec3 outgoingRadiance = vec3(0.0);
    int lightCount = int(camera.cullParams.z);
    for (int i = 0; i < lightCount; ++i) {
        LightData light = lightBuffer.lights[i];

        vec3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight > light.positionRange.w) {
            continue;
        }

        vec3 lightDirection = toLight / distanceToLight;
        vec3 halfway = normalize(viewDirection + lightDirection);

        // 平方反比衰减配合窗口函数，在影响半径处平滑归零
        float distanceRatio = distanceToLight / light.positionRange.w;
        float window = clamp(1.0 - distanceRatio * distanceRatio * distanceRatio * distanceRatio, 0.0, 1.0);
        float attenuation = (window * window) / (distanceToLight * distanceToLight + 1.0);
        vec3 radiance = light.color.rgb * light.color.a * attenuation;

        float normalDistribution = distributionGGX(normal, halfway, roughness);
        float geometry = geometrySmith(normal, viewDirection, lightDirection, roughness);
        vec3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);

        vec3 diffuseRatio = (vec3(1.0) - fresnel) * (1.0 - metallic);

        vec3 numerator = normalDistribution * geometry * fresnel;
        float denominator = 4.0 * max(dot(normal, viewDirection), 0.0) * max(dot(normal, lightDirection), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        float nDotL = max(dot(normal, lightDirection), 0.0);
        outgoingRadiance += (diffuseRatio * albedo / PI + specular) * radiance * nDotL;
    }

    vec3 ambient = vec3(0.12) * albedo * ambientOcclusion;
    vec3 color = ambient + outgoingRadiance;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
