#version 450

// 辐照度图：对每个方向在法线半球上积分，得到漫反射分量

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#include "sky_common.glsl"

void main()
{
    const vec3 normal = uvToDirection(inUv);
    vec3 tangent;
    vec3 bitangent;
    basisFromNormal(normal, tangent, bitangent);

    const uint samples = 512u;
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < samples; ++i) {
        const vec2 xi = hammersley(i, samples);
        const float phi = 2.0 * PI * xi.x;
        const float cosTheta = sqrt(1.0 - xi.y);
        const float sinTheta = sqrt(xi.y);
        const vec3 local = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
        const vec3 direction = tangent * local.x + bitangent * local.y + normal * local.z;
        sum += skyColor(direction);
    }

    outColor = vec4(sum / float(samples), 1.0);
}
