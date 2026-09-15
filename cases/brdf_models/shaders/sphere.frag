#version 450
#extension GL_GOOGLE_include_directive : require

#include "brdf_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 normal = normalize(inWorldNormal);
    const vec3 view = normalize(bb.cameraPosition.xyz - inWorldPosition);
    const float nDotV = max(dot(normal, view), 1e-4);

    const float roughness = max(bb.material.x, 0.02);
    const float metallic = bb.material.y;
    const vec3 baseColor = vec3(bb.material.z);
    const vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    const vec3 diffuseAlbedo = baseColor * (1.0 - metallic);

    vec3 color;
    if (bb.options.x < LIGHTING_FURNACE - 0.5) {
        // 方向光：漫反射项加微表面镜面项，再补上多次散射
        const vec3 lightDirection = normalize(bb.lightDirection.xyz);
        const float nDotL = max(dot(normal, lightDirection), 0.0);
        const vec3 diffuse = diffuseAlbedo / PI;
        vec3 specular = specularBrdf(normal, view, lightDirection, roughness, f0);
        if (bb.options.w > 0.5) {
            specular += multipleScatteringBrdf(nDotV, max(nDotL, 1e-4), roughness, f0);
        }
        color = (diffuse + specular) * bb.lightColor.rgb * bb.lightDirection.w * nDotL;
    } else {
        // 均匀环境：出射辐射亮度是方向反照率乘入射亮度。
        // 单次散射那份要么在片上数值积分，要么直接查系数表；多次散射那份按解析结果补齐：
        // 均匀光照下补偿项对整个半球的积分正好是 F_ms 乘 (1 - E(n·v))
        const float eView = tableDirectionalAlbedo(nDotV, roughness);
        vec3 radiance = diffuseAlbedo;
        if (bb.options.x < LIGHTING_TABLE - 0.5) {
            radiance += integrateSingleScattering(normal, view, roughness, f0, int(bb.miscParams.y));
        } else {
            radiance += vec3(eView);
        }
        if (bb.options.w > 0.5) {
            radiance += multipleScatteringFactor(f0, roughness) * (1.0 - eView);
        }
        color = radiance * bb.lightColor.a;
    }

    outColor = vec4(encodeDisplay(color), 1.0);
}
