#version 450
#extension GL_GOOGLE_include_directive : require

#include "quad_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 outUv;
layout(location = 1) flat out float outAlphaTested;
// 片元阶段拿不到 gl_InstanceIndex，实例编号由顶点阶段带过去
layout(location = 2) flat out uint outInstanceIndex;

void main()
{
    // 绘制顺序由顺序缓冲给出，实例数据本身不随顺序变化
    uint slot = uint(gl_InstanceIndex);
    uint index = orderBuffer.indices[slot];
    InstanceData instance = instanceBuffer.instances[index];

    vec3 worldPosition = instance.positionScale.xyz + vec3(inPosition.xy * instance.positionScale.w, 0.0);

    outUv = inUv;
    outAlphaTested = instance.flags.x;
    outInstanceIndex = index;
    gl_Position = scene.viewProjection * vec4(worldPosition, 1.0);
}
