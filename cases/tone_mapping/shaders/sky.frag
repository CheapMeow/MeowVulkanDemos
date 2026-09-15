#version 450

layout(location = 0) in vec2 inUv;

layout(set = 0, binding = 0) uniform ToneMapBuffer {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;   // xyz 相机位置, w 保留
    vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    vec4 sunDirection;     // xyz 指向太阳的单位向量, w 太阳辐射亮度
    vec4 skyParams;        // x 天空整体亮度, y 天顶到地平线的衰减指数, zw 保留
    vec4 operatorParams;   // x 算子, y 作用方式, z 曝光倍数, w 白点
    vec4 encodingParams;   // x 输出编码, yzw 保留
} tm;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec2 ndc = inUv * 2.0 - 1.0;
    const vec4 farPoint = tm.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    const vec3 direction = normalize(farPoint.xyz / farPoint.w - tm.cameraPosition.xyz);

    // 天顶偏蓝，地平线偏亮，指数控制两者的过渡宽度
    const vec3 zenith = vec3(0.02, 0.04, 0.09);
    const vec3 horizon = vec3(0.35, 0.42, 0.58);
    const float up = max(direction.y, 0.0);
    const vec3 sky = mix(horizon, zenith, pow(up, tm.skyParams.y)) * tm.skyParams.x;

    // 太阳圆盘：内圈是完整的辐射亮度，外圈一条很窄的过渡
    const float cosAngle = dot(direction, normalize(tm.sunDirection.xyz));
    const float disc = smoothstep(0.99955, 0.99985, cosAngle);
    const vec3 sun = vec3(1.0, 0.92, 0.78) * tm.sunDirection.w * disc;

    // 地平线以下按地面处理，亮度低，只有靠近太阳的一侧略微亮一点
    const float below = clamp(-direction.y, 0.0, 1.0);
    const vec3 ground = vec3(0.045, 0.045, 0.05) * tm.skyParams.x * (1.0 + below * disc);

    outColor = vec4(direction.y > 0.0 ? sky + sun : ground, 1.0);
}
