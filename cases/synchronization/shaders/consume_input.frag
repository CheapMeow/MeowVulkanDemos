#version 450

#include "pattern_uniform.glsl"
#include "consume_common.glsl"

layout(location = 0) out vec4 outColor;

layout(input_attachment_index = 0, set = 0, binding = 1) uniform usubpassInput patternInput;

void main()
{
    vec2 uv = gl_FragCoord.xy / uniformData.resolutionAndPhase.xy;
    // 输入附件读的是同一个像素位置上的图案，与最近邻取样得到相同的字节
    uvec3 texel = subpassLoad(patternInput).rgb;
    outColor = vec4(shadePattern(texel, uv), 1.0);
}
