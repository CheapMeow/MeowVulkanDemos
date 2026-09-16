#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;   // xyz 相机位置, w 保留
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 sceneParams;      // x 环境项, yzw 保留
} scene;

void main()
{
    const vec3 normal = normalize(inWorldNormal);
    const vec3 lightDirection = normalize(scene.lightDirection.xyz);
    const float nDotL = max(dot(normal, lightDirection), 0.0);

    // 高动态范围：环境项很弱，主光可以把饱和颜色推到 1 以上
    const vec3 color = inColor * (scene.sceneParams.x + scene.lightDirection.w * nDotL);
    outColor = vec4(color, 1.0);
}
