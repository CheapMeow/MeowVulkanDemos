#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 0) out vec4 outColor;

void main()
{
    if (scene.groundParams.y > 0.5) {
        outColor = vec4(depthVisualizationColor(gl_FragCoord.z), 1.0);
        return;
    }

    vec3 albedo = texture(albedoMap, inUv).rgb;
    vec3 normal = normalize(inWorldNormal);
    float nDotL = max(dot(normal, scene.lightDirection.xyz), 0.0);

    outColor = vec4(albedo * (0.25 + 0.75 * nDotL) * vec3(1.0, 0.96, 0.90), 1.0);
}
