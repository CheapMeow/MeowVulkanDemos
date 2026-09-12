#version 450

layout(location = 0) in vec2 inUvPerspective;
layout(location = 1) noperspective in vec2 inUvAffine;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 options;  // x 插值模式, y 图案, z 棋盘格频率, w 保留
} scene;

layout(location = 0) out vec4 outColor;

// 图案由纹理坐标程序生成，不引入贴图资源
vec3 patternColor(vec2 uv, float pattern, float frequency)
{
    vec2 scaled = uv * frequency;
    if (pattern < 0.5) {
        vec2 cell = floor(scaled);
        float checker = mod(cell.x + cell.y, 2.0);
        return mix(vec3(0.07, 0.08, 0.11), vec3(0.86, 0.88, 0.92), checker);
    }
    // UV 网格：格线位置压暗，格子内部留亮
    vec2 distanceToLine = abs(fract(scaled) - 0.5);
    float line = min(distanceToLine.x, distanceToLine.y);
    return mix(vec3(0.92, 0.93, 0.96), vec3(0.05, 0.06, 0.09), smoothstep(0.0, 0.06, line));
}

void main()
{
    float mode = scene.options.x;
    float pattern = scene.options.y;
    float frequency = scene.options.z;

    vec3 perspectiveColor = patternColor(inUvPerspective, pattern, frequency);
    vec3 affineColor = patternColor(inUvAffine, pattern, frequency);

    if (mode > 1.5) {
        // 差值图：两种插值给出的纹理坐标之差的模长，放大成灰度热力图。
        // 两个端点处两种插值取值相同，误差在图形内部达到最大
        float error = length(inUvPerspective - inUvAffine);
        outColor = vec4(vec3(clamp(error * 1.2, 0.0, 1.0)), 1.0);
        return;
    }

    outColor = vec4(mode > 0.5 ? affineColor : perspectiveColor, 1.0);
}
