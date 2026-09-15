#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(set = 0, binding = 0) uniform ToneMapBuffer {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;   // xyz 相机位置
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 sunDirection;     // xyz 指向太阳的单位向量, w 太阳辐射亮度
    vec4 skyParams;
    vec4 operatorParams;
    vec4 encodingParams;
} tm;

layout(set = 0, binding = 2) uniform sampler2D albedoMap;
layout(set = 0, binding = 3) uniform sampler2D normalMap;

layout(location = 0) out vec4 outColor;

// 用屏幕空间导数构造切线空间，无需模型提供切线数据
vec3 sampleWorldNormal()
{
    vec3 tangentNormal = texture(normalMap, inUv).xyz * 2.0 - 1.0;

    vec3 dPositionX = dFdx(inWorldPosition);
    vec3 dPositionY = dFdy(inWorldPosition);
    vec2 dUvX = dFdx(inUv);
    vec2 dUvY = dFdy(inUv);

    vec3 normal = normalize(inWorldNormal);
    vec3 tangent = normalize(dPositionX * dUvY.t - dPositionY * dUvX.t);
    vec3 bitangent = -normalize(cross(normal, tangent));
    mat3 tangentToWorld = mat3(tangent, bitangent, normal);

    return normalize(tangentToWorld * tangentNormal);
}

void main()
{
    vec3 albedo = texture(albedoMap, inUv).rgb;
    vec3 normal = sampleWorldNormal();

    vec3 lightDirection = normalize(tm.lightDirection.xyz);
    vec3 viewDirection = normalize(tm.cameraPosition.xyz - inWorldPosition);
    vec3 halfway = normalize(viewDirection + lightDirection);

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotH = max(dot(normal, halfway), 0.0);

    // 漫反射与镜面反射都乘上光源强度，强度调到几倍时高光就进入高动态范围
    vec3 diffuse = albedo * nDotL * tm.lightDirection.w;
    vec3 specular = vec3(pow(nDotH, 96.0)) * tm.lightDirection.w * 4.0;
    vec3 ambient = albedo * 0.05;

    outColor = vec4(ambient + diffuse + specular, 1.0);
}
