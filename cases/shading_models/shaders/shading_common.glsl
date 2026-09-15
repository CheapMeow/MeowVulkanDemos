// 光照与法线的公共部分，顶点着色器与片元着色器共用
#ifndef SHADING_MODELS_COMMON_GLSL
#define SHADING_MODELS_COMMON_GLSL

layout(set = 0, binding = 0) uniform ShadingBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;   // xyz 相机位置, w 保留
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 lightColor;       // rgb 光源颜色, a 环境强度
    vec4 options;          // x 镜面模型, y 法线来源, z 高光指数, w 细分段数
} shading;

const float SPECULAR_BLINN_PHONG = 0.0;
const float SPECULAR_PHONG = 1.0;

const float NORMAL_GEOMETRIC = 0.0;
const float NORMAL_BUMP = 1.0;

// 程序化凹凸：高度是两组正弦波的乘积，解析地求出它的两个偏导数当切线空间法线，
// 也就是 h(u,v) = A sin(su) cos(sv) 时的 (-A s cos(su) cos(sv), A s sin(su) sin(sv), 1)
vec3 bumpNormal(vec2 uv)
{
    const float SCALE = 28.0;
    const float AMPLITUDE = 0.035;
    const float u = uv.x * SCALE;
    const float v = uv.y * SCALE;
    const float slopeU = AMPLITUDE * SCALE * cos(u) * cos(v);
    const float slopeV = -AMPLITUDE * SCALE * sin(u) * sin(v);
    return normalize(vec3(-slopeU, -slopeV, 1.0));
}

// 用屏幕空间导数构造切线空间，无需模型提供切线数据。
// 导数只在片元着色器里可用，顶点着色器里的 Gouraud 分支用不上凹凸
#ifdef GL_FRAGMENT_SHADER
vec3 applyBump(vec3 geometricNormal, vec3 worldPosition, vec2 uv)
{
    if (shading.options.y < NORMAL_BUMP - 0.5) {
        return geometricNormal;
    }

    const vec3 dPositionX = dFdx(worldPosition);
    const vec3 dPositionY = dFdy(worldPosition);
    const vec2 dUvX = dFdx(uv);
    const vec2 dUvY = dFdy(uv);

    const vec3 normal = normalize(geometricNormal);
    const vec3 tangent = normalize(dPositionX * dUvY.t - dPositionY * dUvX.t);
    const vec3 bitangent = -normalize(cross(normal, tangent));
    return normalize(mat3(tangent, bitangent, normal) * bumpNormal(uv));
}
#endif

// 完整的着色公式。高光指数越大，高光越集中；两种镜面模型的差别只在半程向量还是反射方向
vec3 shadeSurface(vec3 albedo, vec3 normal, vec3 worldPosition)
{
    // 插值后的法线长度会小于 1，高光指数很大时这点缩短就足以让高光消失
    const vec3 unitNormal = normalize(normal);
    const vec3 lightDirection = normalize(shading.lightDirection.xyz);
    const vec3 viewDirection = normalize(shading.cameraPosition.xyz - worldPosition);
    const float nDotL = max(dot(unitNormal, lightDirection), 0.0);

    float specular;
    if (shading.options.x < SPECULAR_PHONG - 0.5) {
        const vec3 halfway = normalize(viewDirection + lightDirection);
        specular = pow(max(dot(unitNormal, halfway), 0.0), shading.options.z);
    } else {
        const vec3 reflectDirection = reflect(-lightDirection, unitNormal);
        specular = pow(max(dot(reflectDirection, viewDirection), 0.0), shading.options.z);
    }

    const vec3 ambient = albedo * shading.lightColor.a;
    const vec3 diffuse = albedo * nDotL * shading.lightDirection.w;
    const vec3 highlight = vec3(specular) * shading.lightDirection.w;
    return ambient + shading.lightColor.rgb * diffuse + shading.lightColor.rgb * highlight;
}

#endif
