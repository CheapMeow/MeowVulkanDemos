#version 450

#include "pattern_uniform.glsl"
#include "consume_common.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform usampler2D patternSampler;

void main()
{
    vec2 uv = gl_FragCoord.xy / uniformData.resolutionAndPhase.xy;
    // 最近邻取样且取样点落在像素中心，取到的就是同一个像素的图案
    uvec3 texel = texture(patternSampler, uv).rgb;
    outColor = vec4(shadePattern(texel, uv), 1.0);
}
