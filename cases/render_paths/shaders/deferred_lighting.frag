#version 450

// 延迟路径的光照通道：几何信息从几何缓冲读回，光源逐个过一遍

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;   // x 路径, y 光源数量, z 是否用分块列表, w 时间
    vec4 miscParams;   // x 环境光强度, yzw 保留
} scene;

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    vec4 lights[];
} lightBuffer;

layout(set = 0, binding = 5) uniform sampler2D albedoTexture;
layout(set = 0, binding = 6) uniform sampler2D normalTexture;
layout(set = 0, binding = 7) uniform sampler2D positionTexture;

void main()
{
    const vec3 albedo = texture(albedoTexture, inUv).rgb;
    const vec3 normal = normalize(texture(normalTexture, inUv).xyz);
    const vec3 position = texture(positionTexture, inUv).rgb;
    vec3 result = albedo * scene.miscParams.x;

    const uint lightCount = uint(scene.modeParams.y);
    for (uint i = 0u; i < lightCount; ++i) {
        const vec4 lightPosition = lightBuffer.lights[i * 2];
        const vec4 lightColor = lightBuffer.lights[i * 2 + 1];
        const vec3 toLight = lightPosition.xyz - position;
        const float distance = length(toLight);
        if (distance > lightPosition.w) {
            continue;
        }
        const float attenuation = clamp(1.0 - distance / lightPosition.w, 0.0, 1.0);
        result += albedo * lightColor.rgb * lightColor.a * attenuation * attenuation *
                  max(dot(normal, normalize(toLight)), 0.0);
    }

    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
