#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;    // x 遮蔽量乘到哪一档光照, y 采样数, z 采样半径, w 核大小
    vec4 miscParams;    // x 法线加权开关, y 遮蔽强度, z 近平面, w 远平面
} scene;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;
layout(set = 0, binding = 4) uniform sampler2D sceneColor;
layout(set = 0, binding = 5) uniform sampler2D occlusionTexture;

// 遮蔽量乘到哪一档光照
const float APPLY_TO_AMBIENT = 1.0;
const float APPLY_TO_ALL = 2.0;

void main()
{
    const vec3 albedo = texture(sceneColor, inUv).rgb;
    const vec3 normal = normalize(texture(sceneNormal, inUv).xyz);
    const float occlusion = texture(occlusionTexture, inUv).r;

    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    const vec3 ambient = albedo * 0.35;
    const vec3 direct = albedo * 0.85 * diffuse;

    vec3 result;
    if (scene.modeParams.x >= APPLY_TO_ALL - 0.5) {
        // 遮蔽量乘到全部光照：接触阴影与直接光的暗部都会变暗
        result = (ambient + direct) * occlusion;
    } else if (scene.modeParams.x >= APPLY_TO_AMBIENT - 0.5) {
        // 只乘环境光与间接光，直接光不受影响
        result = ambient * occlusion + direct;
    } else {
        result = ambient + direct;
    }

    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
