#version 450

// 基于图像的光照：漫反射查辐照度图，镜面查预滤波环境贴图与查找表。
// 关掉分离求和时直接对原始环境贴图取样，粗糙度不再起作用

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorld;
layout(location = 2) flat in vec3 inColor;
layout(location = 3) flat in vec2 inMaterial;   // x 金属度, y 粗糙度

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;   // x 是否使用分离求和, y 预滤波贴图的层级数, zw 保留
    vec4 miscParams;   // x 探针位置, y 是否开启视差矫正, z 包围形状, w 保留
} scene;

layout(set = 0, binding = 2) uniform sampler2D environmentTexture;
layout(set = 0, binding = 3) uniform sampler2D irradianceTexture;
layout(set = 0, binding = 4) uniform sampler2D brdfTexture;

// 预滤波链的每一级各一张贴图，按粗糙度在它们之间做线性混合
layout(set = 1, binding = 0) uniform sampler2D prefilter0;
layout(set = 1, binding = 1) uniform sampler2D prefilter1;
layout(set = 1, binding = 2) uniform sampler2D prefilter2;
layout(set = 1, binding = 3) uniform sampler2D prefilter3;
layout(set = 1, binding = 4) uniform sampler2D prefilter4;
layout(set = 1, binding = 5) uniform sampler2D prefilter5;

#include "sky_common.glsl"

vec3 samplePrefiltered(vec2 uv, float roughness)
{
    const float level = clamp(roughness, 0.0, 1.0) * 5.0;
    vec3 color = texture(prefilter0, uv).rgb * max(1.0 - level, 0.0);
    color += texture(prefilter1, uv).rgb * max(1.0 - abs(level - 1.0), 0.0);
    color += texture(prefilter2, uv).rgb * max(1.0 - abs(level - 2.0), 0.0);
    color += texture(prefilter3, uv).rgb * max(1.0 - abs(level - 3.0), 0.0);
    color += texture(prefilter4, uv).rgb * max(1.0 - abs(level - 4.0), 0.0);
    color += texture(prefilter5, uv).rgb * max(level - 4.0, 0.0);
    return color;
}

void main()
{
    // 相机在原点朝向 -z
    const vec3 normal = normalize(inNormal);
    const vec3 view = normalize(-inWorld);
    const vec3 reflection = reflect(-view, normal);
    const float nDotV = max(dot(normal, view), 0.001);
    const float metalness = inMaterial.x;
    const float roughness = max(inMaterial.y, 0.02);
    const vec3 albedo = inColor;
    const vec3 f0 = mix(vec3(0.04), albedo, metalness);

    // 探针按无限远环境作假设，视差矫正把取样方向按包围形状修正
    vec3 sampleDirection = normal;
    vec3 specularDirection = reflection;
    if (scene.miscParams.y > 0.5) {
        const vec3 corrected = normalize(normal + (inWorld - scene.miscParams.x * vec3(0.0, 0.0, 1.0)) * 0.08);
        sampleDirection = mix(normal, corrected, 0.6);
        specularDirection = mix(reflection, normalize(reflection + (inWorld) * 0.08), 0.6);
    }

    vec3 diffuse;
    vec3 specular;
    if (scene.modeParams.x > 0.5) {
        diffuse = texture(irradianceTexture, directionToUv(sampleDirection)).rgb * albedo * (1.0 - metalness);
        specular = samplePrefiltered(directionToUv(specularDirection), roughness);
        const vec2 brdf = texture(brdfTexture, vec2(nDotV, roughness)).rg;
        specular *= f0 * brdf.x + brdf.y;
    } else {
        diffuse = texture(environmentTexture, directionToUv(sampleDirection)).rgb * albedo * (1.0 - metalness);
        specular = texture(environmentTexture, directionToUv(specularDirection)).rgb * f0;
    }

    vec3 result = diffuse + specular;
    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
