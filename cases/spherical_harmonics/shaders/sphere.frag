#version 450
#extension GL_GOOGLE_include_directive : require

#include "environment_common.glsl"

layout(location = 0) in vec3 inWorldNormal;

layout(location = 0) out vec4 outColor;

// 漫反射反照率，出射辐射亮度是反照率除以 π 再乘辐照度
const vec3 ALBEDO = vec3(0.75, 0.72, 0.68);

void main()
{
    const vec3 normal = normalize(inWorldNormal);

    vec3 irradiance;
    if (sb.options.z < SHADING_REFERENCE - 0.5) {
        irradiance = shIrradiance(normal, int(sb.options.x));
    } else {
        irradiance = integratedIrradiance(normal, int(sb.options.w));
    }

    outColor = vec4(encodeDisplay(ALBEDO * irradiance / 3.14159265), 1.0);
}
