#version 450

// 预滤波环境贴图：按粗糙度对天空做 GGX 卷积，每一级对应一个粗糙度

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#include "sky_common.glsl"

layout(push_constant) uniform PrefilterParams {
    vec4 params;   // x 粗糙度, yzw 保留
} prefilter;

void main()
{
    const vec3 normal = uvToDirection(inUv);
    const vec3 view = normal;
    const float roughness = clamp(prefilter.params.x, 0.0, 1.0);

    vec3 tangent;
    vec3 bitangent;
    basisFromNormal(normal, tangent, bitangent);

    const uint samples = 256u;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < samples; ++i) {
        const vec2 xi = hammersley(i, samples);
        const float alpha = roughness * roughness;
        const float phi = 2.0 * PI * xi.x;
        const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        const vec3 halfLocal = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
        const vec3 halfVector = tangent * halfLocal.x + bitangent * halfLocal.y + normal * halfLocal.z;

        const vec3 light = 2.0 * dot(view, halfVector) * halfVector - view;
        const float nDotL = max(dot(normal, light), 0.0);
        if (nDotL > 0.0) {
            sum += skyColor(light) * nDotL;
            weight += nDotL;
        }
    }

    outColor = vec4(weight > 0.0 ? sum / weight : vec3(0.0), 1.0);
}
