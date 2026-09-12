// 阴影采样：级联选择、深度比较与切比雪夫估算、PCF 与 PCSS。
// 调用方必须先声明 cascadeMaps 与 scene
#ifndef SHADOW_EXTENDED_SAMPLING_GLSL
#define SHADOW_EXTENDED_SAMPLING_GLSL

// 依次尝试四级，索引写成常量，避免对采样器数组做动态索引
float fetchDepth(int cascade, vec2 uv)
{
    if (cascade == 0) {
        return texture(cascadeMaps[0], uv).r;
    }
    if (cascade == 1) {
        return texture(cascadeMaps[1], uv).r;
    }
    if (cascade == 2) {
        return texture(cascadeMaps[2], uv).r;
    }
    return texture(cascadeMaps[3], uv).r;
}

// 矩模式下每个纹素存一阶与二阶矩，取周围一圈平均当作预滤波
vec2 fetchMoments(int cascade, vec2 uv, float texelSize)
{
    vec2 sum = vec2(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            if (cascade == 0) {
                sum += texture(cascadeMaps[0], uv + offset).rg;
            } else if (cascade == 1) {
                sum += texture(cascadeMaps[1], uv + offset).rg;
            } else if (cascade == 2) {
                sum += texture(cascadeMaps[2], uv + offset).rg;
            } else {
                sum += texture(cascadeMaps[3], uv + offset).rg;
            }
        }
    }
    return sum / 9.0;
}

// 切比雪夫不等式：用矩估算参考深度被遮挡的概率，返回受光比例
float chebyshevVisibility(vec2 moments, float reference)
{
    if (reference <= moments.x) {
        return 1.0;
    }
    float variance = max(moments.y - moments.x * moments.x, 1e-6);
    float delta = reference - moments.x;
    return clamp(variance / (variance + delta * delta), 0.0, 1.0);
}

// 单级阴影。深度模式直接比较，矩模式用切比雪夫公式；两种模式都支持 3x3 的 PCF 与遮挡物搜索。
// 坐标在顶点计算时，第 0 级直接用插值进来的裁剪空间位置，其余级仍按世界位置投影
float sampleCascade(int cascade, vec3 worldPosition, vec3 normal, float nDotL, vec4 lightClip)
{
    vec3 liftedPosition = worldPosition + normal * (scene.shadowOptions.z * scene.shadowOptions.x);
    bool inside = false;
    vec3 projected;
    if (scene.viewOptions.y > 0.5 && cascade == 0) {
        vec3 clip = lightClip.xyz / lightClip.w;
        vec2 uv = clip.xy * 0.5 + 0.5;
        inside = uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && clip.z >= 0.0 && clip.z <= 1.0;
        projected = vec3(uv, clip.z);
    } else {
        projected = shadowProjectionAt(cascade, liftedPosition, inside);
    }
    if (!inside) {
        return 1.0;
    }

    float bias = scene.shadowParams.x;
    if (scene.shadowOptions.y > 0.5) {
        bias = max(bias * (1.0 - nDotL), bias);
    }
    float reference = projected.z - bias;
    float radius = scene.shadowParams.z;
    float texelSize = scene.shadowParams.y;
    bool vsm = scene.viewOptions.x > 0.5;

    if (vsm) {
        vec2 moments = fetchMoments(cascade, projected.xy, texelSize);
        return chebyshevVisibility(moments, reference);
    }

    if (radius <= 0.0) {
        return reference <= fetchDepth(cascade, projected.xy) ? 1.0 : 0.0;
    }

    float blockerRadius = scene.featureOptions.z;
    if (blockerRadius > 0.5) {
        // PCSS：先在较大范围内搜索遮挡物，按平均遮挡深度估计半影大小，再用这个半径做 PCF
        float sum = 0.0;
        float count = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 offset = vec2(float(x), float(y)) * texelSize * blockerRadius;
                float stored = fetchDepth(cascade, projected.xy + offset);
                if (reference > stored) {
                    sum += stored;
                    count += 1.0;
                }
            }
        }
        if (count < 0.5) {
            return 1.0;
        }
        float averageBlocker = sum / count;
        float penumbra = (reference - averageBlocker) * scene.featureOptions.w / max(averageBlocker, 1e-5);
        radius = clamp(penumbra / texelSize * 0.05, 0.5, 8.0);
    }

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texelSize * radius;
            lit += reference <= fetchDepth(cascade, projected.xy + offset) ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

// 视距：世界位置到相机的向量在相机朝向方向上的投影
float viewDepthOf(vec3 worldPosition)
{
    return dot(worldPosition - scene.cameraPosition.xyz, -scene.view[2].xyz);
}

// 返回 0 到 1 的受光比例。阴影开关关闭时恒为 1
float sampleShadow(vec3 worldPosition, vec3 normal, float nDotL, vec4 lightClip)
{
    if (scene.shadowParams.w < 0.5) {
        return 1.0;
    }

    float viewDepth = viewDepthOf(worldPosition);
    int cascade = selectCascade(viewDepth);
    float visibility = sampleCascade(cascade, worldPosition, normal, nDotL, lightClip);

    // 级间融合：在分界附近同时取相邻一级，按落点位置插值，避免分界线上出现硬边
    if (scene.featureOptions.y > 0.5 && cascade + 1 < int(scene.featureOptions.x + 0.5)) {
        float splitNear = cascade == 0 ? 0.0 : scene.cascadeSplits[cascade - 1];
        float splitFar = scene.cascadeSplits[cascade];
        float blendStart = mix(splitNear, splitFar, 0.85);
        float blend = clamp((viewDepth - blendStart) / max(splitFar - blendStart, 1e-4), 0.0, 1.0);
        if (blend > 0.0) {
            float next = sampleCascade(cascade + 1, worldPosition, normal, nDotL, lightClip);
            visibility = mix(visibility, next, blend);
        }
    }
    return visibility;
}

// 级联可视化：每一级一种颜色，亮度随受光比例变化
vec3 cascadeDebugColor(vec3 worldPosition, float visibility)
{
    int cascade = selectCascade(viewDepthOf(worldPosition));
    vec3 palettes[MAX_CASCADE_COUNT] = vec3[MAX_CASCADE_COUNT](
        vec3(1.0, 0.30, 0.25), vec3(0.30, 1.0, 0.40), vec3(0.35, 0.55, 1.0), vec3(1.0, 0.90, 0.30));
    return palettes[cascade] * (0.35 + 0.65 * visibility);
}

#endif
