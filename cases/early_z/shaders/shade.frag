#version 450
#extension GL_GOOGLE_include_directive : require

#include "quad_common.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in float inAlphaTested;
layout(location = 2) flat in uint inInstanceIndex;

layout(location = 0) out vec4 outColor;

void main()
{
    applyDiscard(inUv, inAlphaTested, 0.5);

    InstanceData instance = instanceBuffer.instances[inInstanceIndex];

    outColor = vec4(expensiveShade(inUv, instance.color.rgb, scene.frameParams.x), 1.0);
}
