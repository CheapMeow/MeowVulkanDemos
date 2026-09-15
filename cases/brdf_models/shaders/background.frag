#version 450
#extension GL_GOOGLE_include_directive : require

#include "brdf_common.glsl"

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

void main()
{
    // 炉子测试里背景就是那份均匀环境本身，球的亮度应当与它相等
    vec3 color = vec3(0.0);
    if (bb.options.x > LIGHTING_FURNACE - 0.5) {
        color = bb.lightColor.a * vec3(1.0);
    } else {
        color = vec3(0.05, 0.055, 0.065);
    }
    outColor = vec4(encodeDisplay(color), 1.0);
}
