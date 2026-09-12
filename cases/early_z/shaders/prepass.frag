#version 450
#extension GL_GOOGLE_include_directive : require

#include "quad_common.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in float inAlphaTested;
layout(location = 2) flat in uint inInstanceIndex;

// 深度预通道只写深度，颜色附件不参与，但管线仍然要声明这个输出
layout(location = 0) out vec4 outColor;

void main()
{
    applyDiscard(inUv, inAlphaTested, 0.5);

    // 颜色写入掩码为零，这里的取值不会进入附件，作用是把顶点阶段带过来的实例编号留住，
    // 两条管线的顶点输出与片元输入因此逐项对应
    InstanceData instance = instanceBuffer.instances[inInstanceIndex];
    outColor = vec4(instance.color.rgb, 1.0);
}
