#version 450

// 延迟路径的几何通道：写出反照率、法线与世界坐标，光照留给屏幕空间的全屏通道

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inPosition;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;

void main()
{
    outAlbedo = vec4(inColor, 1.0);
    outNormal = vec4(normalize(inNormal), 1.0);
    outPosition = vec4(inPosition, 1.0);
}
