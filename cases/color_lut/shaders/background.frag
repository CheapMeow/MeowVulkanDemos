#version 450

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;   // xyz 相机位置, w 保留
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 sceneParams;      // x 环境项, yzw 保留
} scene;

void main()
{
    // 背景是一段解析渐变，让调色对色相的影响在整幅画面上都能看出来
    const float up = clamp(inUv.y, 0.0, 1.0);
    const vec3 low = vec3(0.05, 0.08, 0.16);
    const vec3 high = vec3(0.70, 0.55, 0.35);
    outColor = vec4(mix(low, high, pow(up, 1.4)) * 1.2, 1.0);
}
