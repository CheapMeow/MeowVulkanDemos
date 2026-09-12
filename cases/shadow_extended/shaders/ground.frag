#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inLightClip;

#include "shadow_sampling.glsl"
#include "lighting_common.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    // 棋盘格便于观察阴影边界
    float checker = mod(floor(inWorldPosition.x * 0.25) + floor(inWorldPosition.z * 0.25), 2.0);
    vec3 albedo = mix(vec3(0.20, 0.22, 0.26), vec3(0.52, 0.53, 0.57), checker);
    vec3 normal = normalize(inWorldNormal);

    float nDotL = max(dot(normal, scene.lightDirection.xyz), 0.0);
    float visibility = sampleShadow(inWorldPosition, normal, nDotL, inLightClip);

    if (scene.viewOptions.z > 1.5) {
        outColor = vec4(vec3(visibility), 1.0);
        return;
    }
    if (scene.viewOptions.z > 0.5) {
        outColor = vec4(cascadeDebugColor(inWorldPosition, visibility), 1.0);
        return;
    }

    vec3 color = shadeDirectional(inWorldPosition, normal, albedo, 0.85, 0.0, 1.0, visibility);
    outColor = vec4(toneMap(color), 1.0);
}
