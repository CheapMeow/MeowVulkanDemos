// 单向光的直接光照，配合阴影项的受光比例。调用方必须先声明 scene
#ifndef SHADOW_LIGHTING_COMMON_GLSL
#define SHADOW_LIGHTING_COMMON_GLSL

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

// visibility 是阴影项的受光比例
vec3 shadeDirectional(vec3 worldPosition, vec3 normal, vec3 albedo, float roughness, float metallic,
                      float ambientOcclusion, float visibility)
{
    vec3 viewDirection = normalize(scene.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = scene.lightDirection.xyz;
    vec3 halfway = normalize(viewDirection + lightDirection);

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);

    float normalDistribution = distributionGGX(normal, halfway, roughness);
    float geometry = geometrySmith(normal, viewDirection, lightDirection, roughness);

    vec3 numerator = normalDistribution * geometry * fresnel;
    vec3 specular = numerator / (4.0 * nDotV * nDotL + 0.0001);

    vec3 diffuseRatio = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 radiance = scene.lightColor.rgb * scene.lightColor.a;

    vec3 direct = (diffuseRatio * albedo / PI + specular) * radiance * nDotL * visibility;
    vec3 ambient = vec3(0.12) * albedo * ambientOcclusion;

    return ambient + direct;
}

vec3 toneMap(vec3 color)
{
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

#endif
