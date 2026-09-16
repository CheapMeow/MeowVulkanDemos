#version 450

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform DofBuffer {
    vec4 cameraParams;   // x 近平面, y 远平面, z 画面宽度, w 画面高度
    vec4 lensParams;     // x 焦距, y 光圈数, z 对焦距离, w 弥散圆半径上限（像素）
    vec4 modeParams;     // x 处理方式, y 采样数, z 抖动强度, w 光圈叶片数（零表示圆形）
    vec4 miscParams;     // x 曝光倍数, y 钳制系数, zw 保留
} dof;

layout(set = 0, binding = 1) uniform sampler2D sceneColor;
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;

const float MODE_OFF = 0.0;
const float MODE_GAUSSIAN = 1.0;
const float MODE_DISK = 2.0;

const float PI = 3.14159265359;

// 由深度缓冲里的归一化深度还原视空间距离
float viewDistance(vec2 uv)
{
    const float depth = texture(sceneDepth, uv).r;
    const float near = dof.cameraParams.x;
    const float far = dof.cameraParams.y;
    return near * far / (far - depth * (far - near));
}

// 薄透镜的弥散圆，换算成像素半径。感光面宽度与镜头参数使用同一套世界单位
float circleOfConfusion(float distance)
{
    const float sensorWidth = 0.036;
    const float focalLength = dof.lensParams.x;
    const float fNumber = dof.lensParams.y;
    const float focusDistance = dof.lensParams.z;
    const float denominator = fNumber * distance * (focusDistance - focalLength);
    if (abs(denominator) < 1e-6) {
        return 0.0;
    }
    const float diameter = focalLength * focalLength * (distance - focusDistance) / denominator;
    return 0.5 * diameter / sensorWidth * dof.cameraParams.z;
}

// 弥散圆的半径上限，同时钳制到画面短边的一半
float clampedRadius(float coc)
{
    return min(abs(coc), dof.lensParams.w);
}

// 圆盘上的采样点：黄金角螺旋，叶片数大于零时按正多边形裁剪
vec2 diskSample(int index, int count, float jitter)
{
    const float goldenAngle = 2.39996323;
    const float radius = sqrt((float(index) + 0.5) / float(count));
    const float angle = float(index) * goldenAngle + jitter * 6.2831853;
    vec2 point = vec2(cos(angle), sin(angle)) * radius;

    const float blades = dof.modeParams.w;
    if (blades > 0.5) {
        // 正多边形裁剪：半径沿着角度方向的边界由内切圆半径与顶点半径决定
        const float sector = 2.0 * PI / blades;
        float local = mod(atan(point.y, point.x) + jitter * sector, sector) - sector * 0.5;
        const float boundary = cos(sector * 0.5) / cos(local);
        point /= max(boundary, 1e-4);
    }
    return point;
}

vec3 toneMapAndEncode(vec3 color)
{
    color *= dof.miscParams.x;
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    const vec3 mapped = clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
    const vec3 low = mapped * 12.92;
    const vec3 high = 1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), mapped));
}

void main()
{
    const vec2 texel = 1.0 / dof.cameraParams.zw;
    const vec3 center = texture(sceneColor, inUv).rgb;

    if (dof.modeParams.x < MODE_GAUSSIAN - 0.5) {
        outColor = vec4(toneMapAndEncode(center), 1.0);
        return;
    }

    const float centerCoc = circleOfConfusion(viewDistance(inUv));
    const float centerRadius = clampedRadius(centerCoc);

    if (dof.modeParams.x < MODE_DISK - 0.5) {
        // 普通做法：按中心像素的弥散圆铺一圈固定的权重，不看邻居的深度
        const int taps = 3;
        vec3 accum = vec3(0.0);
        float weightSum = 0.0;
        for (int y = -taps; y <= taps; ++y) {
            for (int x = -taps; x <= taps; ++x) {
                const vec2 offset = vec2(float(x), float(y)) / float(taps);
                const float weight = exp(-dot(offset, offset) * 2.5);
                const vec2 uv = inUv + offset * centerRadius * texel;
                accum += texture(sceneColor, uv).rgb * weight;
                weightSum += weight;
            }
        }
        outColor = vec4(toneMapAndEncode(accum / weightSum), 1.0);
        return;
    }

    // 弥散圆感知的收集：每个采样点先算自己的弥散圆，够大才认为它能覆盖到本像素
    const int sampleCount = int(dof.modeParams.y);
    vec3 accum = center;
    float weightSum = 1.0;
    for (int i = 0; i < sampleCount; ++i) {
        const vec2 offset = diskSample(i, sampleCount, dof.modeParams.z) * centerRadius;
        const float distance = length(offset);
        if (distance < 1e-4) {
            continue;
        }
        const vec2 uv = inUv + offset * texel;
        const float sampleCoc = clampedRadius(circleOfConfusion(viewDistance(uv)));
        if (sampleCoc < distance) {
            continue;
        }
        accum += texture(sceneColor, uv).rgb;
        weightSum += 1.0;
    }
    outColor = vec4(toneMapAndEncode(accum / weightSum), 1.0);
}
