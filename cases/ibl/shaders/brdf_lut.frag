#version 450

// 查找表：把法线与视线夹角、粗糙度这一对参数对应的镜面响应预先积出来，
// 横轴是法线与视线的夹角余弦，纵轴是粗糙度

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#include "sky_common.glsl"

float geometrySchlickGGX(float nDotV, float roughness)
{
    const float k = (roughness * roughness) / 2.0;
    return nDotV / (nDotV * (1.0 - k) + k);
}

void main()
{
    const float nDotV = clamp(inUv.x, 0.001, 1.0);
    const float roughness = clamp(inUv.y, 0.001, 1.0);

    const vec3 normal = vec3(0.0, 0.0, 1.0);
    const vec3 view = vec3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
    vec3 tangent;
    vec3 bitangent;
    basisFromNormal(normal, tangent, bitangent);

    const uint samples = 512u;
    float scale = 0.0;
    float bias = 0.0;
    for (uint i = 0u; i < samples; ++i) {
        const vec2 xi = hammersley(i, samples);
        const float alpha = roughness * roughness;
        const float phi = 2.0 * PI * xi.x;
        const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        const vec3 halfLocal = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
        const vec3 halfVector = tangent * halfLocal.x + bitangent * halfLocal.y + normal * halfLocal.z;

        const vec3 light = 2.0 * dot(view, halfVector) * halfVector - view;
        const float nDotL = max(light.z, 0.0);
        const float nDotH = max(halfVector.z, 0.0);
        const float vDotH = max(dot(view, halfVector), 0.0);
        if (nDotL <= 0.0) {
            continue;
        }

        const float geometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
        const float visibility = geometry / (nDotV * nDotL + 1e-4);
        const float fresnel = pow(1.0 - vDotH, 5.0);
        scale += (1.0 - fresnel) * visibility * nDotL;
        bias += fresnel * visibility * nDotL;
    }

    outColor = vec4(scale / float(samples), bias / float(samples), 0.0, 1.0);
}
