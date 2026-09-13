#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outGlow;
layout(location = 1) out vec4 outGeometry;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;
    vec4 miscParams;   // x 时间, y 光斑强度, z 近平面, w 远平面
} scene;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float viewDepth(float ndcDepth)
{
    const float near = scene.miscParams.z;
    const float far = scene.miscParams.w;
    return near / max(1.0 - ndcDepth * (1.0 - near / far), 1e-5);
}

vec3 layerField(vec2 uv)
{
    const float z = viewDepth(texture(sceneDepth, uv).r);
    const float haze = pow(clamp(1.0 - z / 8.0, 0.0, 1.0), 1.5);
    vec3 color = vec3(0.55, 0.70, 0.95) * haze * 1.8;

    for (int i = 0; i < 5; ++i) {
        float fi = float(i);
        float phase = scene.miscParams.x;
        vec2 center = vec2(0.5 + 0.34 * sin(phase * 0.31 + fi * 1.7),
                           0.5 + 0.30 * cos(phase * 0.24 + fi * 2.3));
        float radius = 0.18 + 0.10 * hash11(fi * 3.7 + 1.0);
        vec2 delta = uv - center;
        color += vec3(0.60, 0.50, 0.90) * exp(-dot(delta, delta) / (radius * radius)) * 0.18;
    }
    return color * scene.miscParams.y;
}

// 光斑与低分辨率几何在同一个子通道里写出，不必再跑一趟单独的降采样
void main()
{
    outGlow = vec4(layerField(inUv), 1.0);
    outGeometry = vec4(texture(sceneDepth, inUv).r, texture(sceneNormal, inUv).xyz);
}
