#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outGeometry;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;

// 把全分辨率深度与法线降到低分辨率，供联合双边上采样加权使用
void main()
{
    float depth = texture(sceneDepth, inUv).r;
    vec3 normal = texture(sceneNormal, inUv).xyz;
    outGeometry = vec4(depth, normal);
}
