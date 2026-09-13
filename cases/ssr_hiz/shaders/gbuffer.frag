#version 450

// 几何缓冲：输出着色后的颜色与粗糙度，以及视空间法线与反射强度

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec4 inColorReflectivity;
layout(location = 3) in float inRoughness;

layout(location = 0) out vec4 outColorRoughness;
layout(location = 1) out vec4 outNormalReflectivity;

layout(set = 0, binding = 0) uniform SsrUniform {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseProjection;
    vec4 viewportParams;
    vec4 marchParams;
    vec4 modeParams;
    vec4 miscParams;
    vec4 frameParams;
    vec4 rangeParams;
} ssr;

void main()
{
    const vec3 normal = normalize(inWorldNormal);
    const vec3 albedo = inColorReflectivity.rgb;

    const vec3 lightDirection = normalize(vec3(0.45, 0.85, 0.30));
    const float diffuse = max(dot(normal, lightDirection), 0.0);
    const vec3 color = albedo * (0.20 + 0.90 * diffuse);

    outColorRoughness = vec4(color, inRoughness);
    outNormalReflectivity = vec4(normalize((ssr.view * vec4(normal, 0.0)).xyz), inColorReflectivity.a);
}
