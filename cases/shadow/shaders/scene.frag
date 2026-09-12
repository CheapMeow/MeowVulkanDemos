#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicMap;
layout(set = 1, binding = 3) uniform sampler2D roughnessMap;
layout(set = 1, binding = 4) uniform sampler2D ambientOcclusionMap;
layout(set = 1, binding = 5) uniform sampler2DShadow shadowMap;

#include "shadow_sampling.glsl"
#include "lighting_common.glsl"

layout(location = 0) out vec4 outColor;

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
    // 贴图以 sRGB 存储，采样时已经转到线性空间，直接用即可
    vec3 albedo = texture(albedoMap, inUv).rgb;
    float ambientOcclusion = texture(ambientOcclusionMap, inUv).r;
    float metallic = texture(metallicMap, inUv).r;
    float roughness = clamp(texture(roughnessMap, inUv).r, 0.04, 1.0);
    vec3 normal = sampleWorldNormal();

    float nDotL = max(dot(normal, scene.lightDirection.xyz), 0.0);
    float visibility = sampleShadow(inWorldPosition, normal, nDotL);

    vec3 color = shadeDirectional(inWorldPosition, normal, albedo, roughness, metallic, ambientOcclusion,
                                  visibility);
    outColor = vec4(toneMap(color), 1.0);
}
