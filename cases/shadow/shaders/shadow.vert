#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;

void main()
{
    // 阴影通道只需要深度，物体与地面都用各自的实例变换
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];
    mat3 rotation = rotationAroundY(instance.rotation.x);
    vec3 worldPosition = rotation * (inPosition * instance.positionScale.w) + instance.positionScale.xyz;

    gl_Position = scene.lightViewProj * vec4(worldPosition, 1.0);
}
