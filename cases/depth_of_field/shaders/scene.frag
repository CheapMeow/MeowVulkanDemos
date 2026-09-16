#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inEmission;

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

    const float attenuation = 1.0 / (1.0 + 0.004 * dot(inWorldPosition, inWorldPosition) * 0.1);
    const vec3 lit = inColor * (scene.sceneParams.x + scene.lightDirection.w * nDotL) * attenuation;
    // 自发光的小球远高于显示范围，景深会把它们摊成散景光斑
    outColor = vec4(lit + inColor * inEmission, 1.0);
}
