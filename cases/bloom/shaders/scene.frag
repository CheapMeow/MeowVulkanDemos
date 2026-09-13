#version 450

// 高动态范围场景：暗背景上有一个缓慢移动的亮光源球，另加几条细亮条用来观察抗锯齿与
// 色调映射的先后顺序

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform BloomBuffer {
    vec4 viewportParams;   // xy 全屏尺寸, zw 保留
    vec4 modeParams;       // x 阈值模式, y 链实现, z 是否应用阈值, w 层数
    vec4 miscParams;       // x 阈值, y 泛光强度, z 时间, w 是否抖动
} bloom;

void main()
{
    const vec2 uv = inUv;
    vec3 color = vec3(0.004, 0.006, 0.010);

    // 缓慢移动的光源球，亮度远高于 1
    const vec2 center = vec2(0.5 + 0.28 * sin(bloom.miscParams.z * 0.37),
                             0.5 + 0.22 * cos(bloom.miscParams.z * 0.29));
    const float distanceToLight = length((uv - center) * vec2(1.78, 1.0));
    color += vec3(9.0, 7.4, 5.2) * exp(-distanceToLight * distanceToLight / 0.0035);

    // 几条细亮条，宽度不足一个像素，用来对照抗锯齿与色调映射的先后
    const float bars = abs(fract(uv.x * 260.0) - 0.5);
    color += vec3(1.6, 1.5, 1.4) * smoothstep(0.06, 0.0, bars) * step(uv.y, 0.18);

    outColor = vec4(color, 1.0);
}
