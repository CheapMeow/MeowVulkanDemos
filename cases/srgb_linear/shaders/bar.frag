#version 450

layout(location = 0) in float inValue;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 options;  // x 反照率按 sRGB 解释, y 法线按 sRGB 解释, z 输出编码, w 色调映射
} scene;

layout(location = 0) out vec4 outColor;

void main()
{
    // 参考条只经过与物体相同的色调映射与输出编码，用来把像素值跟理论值对照
    float value = inValue;
    if (scene.options.w > 0.5 && scene.options.w < 1.5) {
        value = value / (value + 1.0);
    } else if (scene.options.w > 1.5) {
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        value = clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0, 1.0);
    }
    if (scene.options.z > 0.5) {
        value = pow(value, 1.0 / 2.2);
    }
    outColor = vec4(vec3(value), 1.0);
}
