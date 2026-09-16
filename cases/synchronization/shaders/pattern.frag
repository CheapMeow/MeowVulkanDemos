#version 450

#include "pattern_uniform.glsl"
#include "pattern_common.glsl"

layout(location = 0) out uvec4 outTexel;

void main()
{
    // 像素中心坐标与计算着色器里的 gl_GlobalInvocationID + 0.5 一致
    outTexel = patternTexel(gl_FragCoord.xy, uniformData.resolutionAndPhase.xy,
                            uniformData.resolutionAndPhase.z, uniformData.resolutionAndPhase.w);
}
