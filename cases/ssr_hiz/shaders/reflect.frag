#version 450

// 屏幕空间反射：由深度重建视空间位置，视空间法线给出反射方向，沿方向步进，
// 命中后采样场景颜色。三种步进方式用特化常量选择，运行时只保留其中一条分支

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outReflection;    // rgb 反射颜色乘权重, a 权重
layout(location = 1) out float outStepFraction; // 步进次数除以步数上限

layout(constant_id = 0) const int MARCH_MODE = 0;   // 0 视空间等距, 1 屏幕像素, 2 层次遍历

layout(set = 0, binding = 0) uniform SsrUniform {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseProjection;
    vec4 viewportParams;   // xy 视口尺寸, z 金字塔层级数, w 是否启用反射
    vec4 marchParams;      // x 步数上限, y 步长, z 厚度阈值, w 抖动强度
    vec4 modeParams;       // x 步进模式, y 二分细化, z 累积混合系数, w 抖动开关
    vec4 miscParams;       // x 可视化模式, y 可视化层级, z 屏幕边缘淡出, w 用平均金字塔
    vec4 frameParams;      // x 帧号, y 是否重置累积, z 近平面, w 远平面
    vec4 rangeParams;      // x 反射推进的最大视空间距离, yzw 保留
} ssr;

layout(set = 0, binding = 2) uniform sampler2D sceneColor;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;
layout(set = 0, binding = 4) uniform sampler2D sceneNormal;
layout(set = 0, binding = 5) uniform sampler2D pyramidMin;
layout(set = 0, binding = 6) uniform sampler2D pyramidAvg;
layout(set = 0, binding = 7) uniform sampler2D previousReflection;

float hash01(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return float(value & 0xffffffu) / float(0x1000000u);
}

// 由深度缓冲重建视空间位置
vec3 reconstructViewPosition(vec2 uv, float depth)
{
    const vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    const vec4 view = ssr.inverseProjection * clip;
    return view.xyz / view.w;
}

// 投影到屏幕，返回归一化设备坐标下的深度
bool projectToScreen(vec3 viewPosition, out vec2 uv, out float ndcDepth)
{
    const vec4 clip = ssr.projection * vec4(viewPosition, 1.0);
    if (clip.w <= 0.0) {
        uv = vec2(0.0);
        ndcDepth = 1.0;
        return false;
    }
    ndcDepth = clip.z / clip.w;
    uv = (clip.xy / clip.w) * 0.5 + 0.5;
    return true;
}

// 投影后的深度换算成视空间距离
float viewDistance(float ndcDepth)
{
    const float nearPlane = ssr.frameParams.z;
    const float farPlane = ssr.frameParams.w;
    return (nearPlane * farPlane) / (farPlane - ndcDepth * (farPlane - nearPlane));
}

// 命中判定：射线走到几何之后，且两者在视空间的距离差落在厚度阈值之内
bool acceptHit(float rayDepth, float sceneDepth, out float gap)
{
    gap = viewDistance(rayDepth) - viewDistance(sceneDepth);
    return gap > 0.02 && gap < ssr.marchParams.z;
}

struct TraceResult {
    bool hit;
    vec2 uv;
    vec2 previousUv;
    float rayDepth;
    float previousRayDepth;
    int steps;
};

TraceResult makeMiss()
{
    TraceResult result;
    result.hit = false;
    result.uv = vec2(0.0);
    result.previousUv = vec2(0.0);
    result.rayDepth = 0.0;
    result.previousRayDepth = 0.0;
    result.steps = 0;
    return result;
}

// 把射线的起点与终点投影到屏幕，得到屏幕直线。射线朝相机方向时先截断在近平面之前
bool computeScreenSegment(vec3 origin, vec3 direction, float maxDistance, out vec2 startUv,
                          out float startDepth, out vec2 endUv, out float endDepth, out float pixelLength)
{
    startUv = vec2(0.0);
    endUv = vec2(0.0);
    startDepth = 0.0;
    endDepth = 0.0;
    pixelLength = 0.0;

    float distance = maxDistance;
    if (direction.z > 0.0) {
        distance = min(distance, max((-0.06 - origin.z) / direction.z, 0.0));
    }
    if (!projectToScreen(origin, startUv, startDepth)) {
        return false;
    }
    if (!projectToScreen(origin + direction * distance, endUv, endDepth)) {
        return false;
    }
    // 射线从表面上出发，起点深度整体往相机一侧偏一点。
    // 不偏的话射线在前几块里与自身表面几乎同深，整块判定会反复失败，
    // 层次遍历退化成一格一格地降级
    const float depthBias = 0.0004;
    startDepth = max(startDepth - depthBias, 0.0);
    endDepth = max(endDepth - depthBias, 0.0);
    pixelLength = length((endUv - startUv) * ssr.viewportParams.xy);
    return pixelLength > 1.0;
}

// 视空间等距步进：每一步固定长度，走完一步投影到屏幕查一次深度
TraceResult marchViewSpace(vec3 origin, vec3 direction)
{
    TraceResult result = makeMiss();
    const int maxSteps = int(ssr.marchParams.x);
    const float stepLength = ssr.marchParams.y;
    const float maxDistance = ssr.rangeParams.x;

    vec3 position = origin;
    vec2 previousUv = vec2(0.0);
    float previousDepth = 0.0;
    bool hasPrevious = false;

    for (int i = 0; i < maxSteps; ++i) {
        if (float(i + 1) * stepLength > maxDistance) {
            break;
        }
        position += direction * stepLength;
        vec2 uv;
        float rayDepth;
        if (!projectToScreen(position, uv, rayDepth)) {
            break;
        }
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            break;
        }
        result.steps = i + 1;
        const float sceneDepth = texture(sceneDepth, uv).r;
        float gap;
        if (rayDepth > sceneDepth && acceptHit(rayDepth, sceneDepth, gap)) {
            result.hit = true;
            result.uv = uv;
            result.previousUv = hasPrevious ? previousUv : uv;
            result.rayDepth = rayDepth;
            result.previousRayDepth = hasPrevious ? previousDepth : rayDepth;
            break;
        }
        previousUv = uv;
        previousDepth = rayDepth;
        hasPrevious = true;
    }
    return result;
}

// 屏幕空间按像素步进：在屏幕直线上按固定像素数推进，深度在投影后的归一化设备坐标
// 的 z 上线性插值。z 在屏幕空间本身就是线性的，插出来的深度与真实射线完全一致
TraceResult marchScreenPixels(vec3 origin, vec3 direction)
{
    TraceResult result = makeMiss();
    const int maxSteps = int(ssr.marchParams.x);
    const float stepPixels = ssr.marchParams.y;

    vec2 startUv;
    vec2 endUv;
    float startDepth;
    float endDepth;
    float pixelLength;
    if (!computeScreenSegment(origin, direction, ssr.rangeParams.x, startUv, startDepth, endUv, endDepth,
                              pixelLength)) {
        return result;
    }

    const int stepCount = min(int(ceil(pixelLength / stepPixels)), maxSteps);
    float previousT = 0.0;
    for (int i = 1; i <= stepCount; ++i) {
        const float t = min(float(i) * stepPixels / pixelLength, 1.0);
        const vec2 uv = mix(startUv, endUv, t);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            break;
        }
        const float rayDepth = mix(startDepth, endDepth, t);
        result.steps = i;
        const float sceneDepth = texture(sceneDepth, uv).r;
        float gap;
        if (rayDepth > sceneDepth && acceptHit(rayDepth, sceneDepth, gap)) {
            result.hit = true;
            result.uv = uv;
            result.previousUv = mix(startUv, endUv, previousT);
            result.rayDepth = rayDepth;
            result.previousRayDepth = mix(startDepth, endDepth, previousT);
            break;
        }
        previousT = t;
    }
    return result;
}

// 查一个格子：解出射线离开格子的位置，给出进出两点的射线深度，再取这一块里最靠前的深度
void queryCell(vec2 position, float traveled, float cellSize, int level, vec2 depthRange,
               float pixelLength, vec2 directionPixel, vec2 screenSize, bool useAverage,
               out float exitStep, out float exitTraveled, out float entryDepth, out float exitDepth,
               out float cellDepth)
{
    const vec2 cellIndex = floor(position / cellSize);
    const vec2 cellMinCorner = cellIndex * cellSize;
    const vec2 cellMaxCorner = cellMinCorner + cellSize;

    vec2 exitDistance = vec2(1e9);
    if (directionPixel.x > 0.0) {
        exitDistance.x = (cellMaxCorner.x - position.x) / directionPixel.x;
    } else if (directionPixel.x < 0.0) {
        exitDistance.x = (cellMinCorner.x - position.x) / directionPixel.x;
    }
    if (directionPixel.y > 0.0) {
        exitDistance.y = (cellMaxCorner.y - position.y) / directionPixel.y;
    } else if (directionPixel.y < 0.0) {
        exitDistance.y = (cellMinCorner.y - position.y) / directionPixel.y;
    }
    exitStep = max(min(exitDistance.x, exitDistance.y), 0.0);
    exitTraveled = min(traveled + exitStep, pixelLength);

    entryDepth = mix(depthRange.x, depthRange.y, traveled / pixelLength);
    exitDepth = mix(depthRange.x, depthRange.y, exitTraveled / pixelLength);

    const vec2 cellCount = ceil(screenSize / cellSize);
    const vec2 clampedCell = clamp(cellIndex, vec2(0.0), cellCount - vec2(1.0));
    const vec2 uvTexel = ((clampedCell + vec2(0.5)) * cellSize) / screenSize;
    cellDepth = useAverage ? textureLod(pyramidAvg, uvTexel, float(level)).r
                           : textureLod(pyramidMin, uvTexel, float(level)).r;
}

// 层次遍历：每一级先整块判断射线是否整段都在这一块的最前面，是就跳过整块，否就下降一级。
// 跳过之后只向上试探一级，探不动就留在原级继续跳，不必退回来重来。
// 计数按纹理读取次数算，这样与线性步进的一次一步可以直接比较
TraceResult marchHiZ(vec3 origin, vec3 direction)
{
    TraceResult result = makeMiss();
    const int maxSteps = int(ssr.marchParams.x);
    const bool useAverage = ssr.miscParams.w > 0.5;

    vec2 startUv;
    vec2 endUv;
    float startDepth;
    float endDepth;
    float pixelLength;
    if (!computeScreenSegment(origin, direction, ssr.rangeParams.x, startUv, startDepth, endUv, endDepth,
                              pixelLength)) {
        return result;
    }

    const vec2 screenSize = ssr.viewportParams.xy;
    const vec2 startPixel = startUv * screenSize;
    const vec2 endPixel = endUv * screenSize;
    const vec2 directionPixel = (endPixel - startPixel) / pixelLength;
    const vec2 depthRange = vec2(startDepth, endDepth);

    const int maxLevel = max(int(ssr.viewportParams.z) - 1, 0);
    int level = maxLevel;
    float cellSize = exp2(float(level));
    vec2 position = startPixel;
    float traveled = 0.0;
    float previousTraveled = 0.0;

    while (result.steps < maxSteps && traveled < pixelLength) {
        if (position.x < 0.0 || position.x > screenSize.x || position.y < 0.0 ||
            position.y > screenSize.y) {
            break;
        }

        float exitStep;
        float exitTraveled;
        float entryDepth;
        float exitDepth;
        float cellDepth;
        queryCell(position, traveled, cellSize, level, depthRange, pixelLength, directionPixel,
                  screenSize, useAverage, exitStep, exitTraveled, entryDepth, exitDepth, cellDepth);
        ++result.steps;

        if (max(entryDepth, exitDepth) <= cellDepth) {
            // 整块都在射线前面，跳过整块
            previousTraveled = traveled;
            traveled = exitTraveled + 1.0;
            position += directionPixel * (exitStep + 1.0);

            // 向上试探一级：新位置上再查一次，通过就换用更大的格子
            if (level < maxLevel && traveled < pixelLength) {
                const float parentSize = cellSize * 2.0;
                float parentExitStep;
                float parentExitTraveled;
                float parentEntryDepth;
                float parentExitDepth;
                float parentDepth;
                queryCell(position, traveled, parentSize, level + 1, depthRange, pixelLength,
                          directionPixel, screenSize, useAverage, parentExitStep, parentExitTraveled,
                          parentEntryDepth, parentExitDepth, parentDepth);
                ++result.steps;
                if (max(parentEntryDepth, parentExitDepth) <= parentDepth) {
                    level = level + 1;
                    cellSize = parentSize;
                }
            }
        } else if (level > 0) {
            // 这一块里可能有交点，下降到细一级再看
            level -= 1;
            cellSize = exp2(float(level));
        } else {
            // 最细一级按像素逐个判定
            const vec2 pixelUv = clamp(position / screenSize, vec2(0.0), vec2(1.0));
            const float pixelDepth = textureLod(sceneDepth, pixelUv, 0.0).r;
            ++result.steps;
            float gap;
            if (entryDepth > pixelDepth && acceptHit(entryDepth, pixelDepth, gap)) {
                result.hit = true;
                result.uv = pixelUv;
                result.previousUv =
                    clamp((position - directionPixel) / screenSize, vec2(0.0), vec2(1.0));
                result.rayDepth = entryDepth;
                result.previousRayDepth = mix(startDepth, endDepth, previousTraveled / pixelLength);
                break;
            }
            previousTraveled = traveled;
            traveled += 1.0;
            position += directionPixel;
            level = min(level + 1, maxLevel);
            cellSize = exp2(float(level));
        }
    }
    return result;
}

// 二分细化：只在命中区间两端反复取中点，把交点收敛到亚像素
void refineHit(inout TraceResult result)
{
    if (!result.hit || ssr.modeParams.y < 0.5) {
        return;
    }
    for (int i = 0; i < 5; ++i) {
        const vec2 midUv = (result.previousUv + result.uv) * 0.5;
        const float midDepth = (result.previousRayDepth + result.rayDepth) * 0.5;
        const float sceneDepth = texture(sceneDepth, midUv).r;
        if (midDepth > sceneDepth) {
            result.uv = midUv;
            result.rayDepth = midDepth;
        } else {
            result.previousUv = midUv;
            result.previousRayDepth = midDepth;
        }
    }
}

void main()
{
    const float depth = texture(sceneDepth, inUv).r;
    const vec4 normalReflectivity = texture(sceneNormal, inUv);
    const float reflectivity = normalReflectivity.a;

    vec4 reflection = vec4(0.0);
    float stepFraction = 0.0;

    if (ssr.viewportParams.w > 0.5 && depth < 1.0 && reflectivity > 0.0) {
        const vec3 viewPosition = reconstructViewPosition(inUv, depth);
        const vec3 viewNormal = normalize(normalReflectivity.xyz);
        vec3 rayDirection = reflect(normalize(viewPosition), viewNormal);

        // 粗糙度抖动：把反射方向在锥内扰动，配合时域累积用时间换采样率
        if (ssr.modeParams.w > 0.5) {
            const float roughness = texture(sceneColor, inUv).a;
            const uint pixelIndex = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u;
            const float random = hash01(pixelIndex + uint(ssr.frameParams.x) * 26699u);
            const float angle = random * 6.2831853;
            const vec3 axisA = normalize(cross(rayDirection, vec3(0.013, 1.0, 0.007)));
            const vec3 axisB = cross(rayDirection, axisA);
            rayDirection =
                normalize(rayDirection + (axisA * cos(angle) + axisB * sin(angle)) *
                                             (roughness * ssr.marchParams.w));
        }

        TraceResult trace;
        if (MARCH_MODE == 0) {
            trace = marchViewSpace(viewPosition, rayDirection);
        } else if (MARCH_MODE == 1) {
            trace = marchScreenPixels(viewPosition, rayDirection);
        } else {
            trace = marchHiZ(viewPosition, rayDirection);
        }
        refineHit(trace);

        stepFraction = clamp(float(trace.steps) / max(ssr.marchParams.x, 1.0), 0.0, 1.0);

        if (trace.hit) {
            const vec3 hitColor = texture(sceneColor, trace.uv).rgb;
            const float edgeFactor = clamp(ssr.miscParams.z, 0.0, 0.5);
            float fade = 1.0;
            if (edgeFactor > 0.0) {
                const float border =
                    min(min(trace.uv.x, 1.0 - trace.uv.x), min(trace.uv.y, 1.0 - trace.uv.y));
                fade = smoothstep(0.0, edgeFactor, border);
            }
            const float weight = reflectivity * fade;
            reflection = vec4(hitColor * weight, weight);
        }
    }

    // 时域累积：与上一帧的结果混合，重置的那一帧直接采用当前值
    const vec4 previous = texture(previousReflection, inUv);
    const float blend = ssr.frameParams.y > 0.5 ? 1.0 : clamp(ssr.modeParams.z, 0.0, 1.0);
    outReflection = mix(previous, reflection, blend);
    outStepFraction = stepFraction;
}
