#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D metallicMap;
layout(set = 2, binding = 3) uniform sampler2D roughnessMap;
layout(set = 2, binding = 4) uniform sampler2D ambientOcclusionMap;

layout(location = 0) out vec4 outAlbedoOcclusion;
layout(location = 1) out vec4 outNormalRoughness;
layout(location = 2) out vec4 outPositionMetallic;

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
    float ambientOcclusion = texture(ambientOcclusionMap, inUv).r;
    float metallic = texture(metallicMap, inUv).r;
    float roughness = texture(roughnessMap, inUv).r;

    outAlbedoOcclusion = vec4(albedo, ambientOcclusion);
    outNormalRoughness = vec4(sampleWorldNormal(), roughness);
    outPositionMetallic = vec4(inWorldPosition, metallic);
}
