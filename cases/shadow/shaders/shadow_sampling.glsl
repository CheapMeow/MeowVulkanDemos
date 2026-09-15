// 阴影采样。调用方必须先声明 shadowMap 与 scene
#ifndef SHADOW_SAMPLING_GLSL
#define SHADOW_SAMPLING_GLSL

// 阴影贴图把深度值存在颜色附件里，比较在着色器里手动完成：
// 参考深度不大于贴图里存的深度就算受光，否则处在阴影里
float litFromDepth(float referenceDepth, vec2 uv)
{
    return referenceDepth <= texture(shadowMap, uv).r ? 1.0 : 0.0;
}

// 百分比渐近过滤：在半径上取 3 乘 3 个纹素各比较一次再取平均。
// 半径为零时退化成一次比较，得到硬阴影
float filterShadow(float referenceDepth, vec2 uv, float radius)
{
    if (radius <= 0.0) {
        return litFromDepth(referenceDepth, uv);
    }

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 offset = vec2(float(x), float(y)) * scene.shadowParams.y * radius;
            sum += litFromDepth(referenceDepth, uv + offset);
        }
    }
    return sum / 9.0;
}

// 在推算出的半影宽度上过滤：5 乘 5 个采样覆盖正负一个半影
float filterPenumbra(float referenceDepth, vec2 uv, float penumbra)
{
    float sum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const vec2 offset = vec2(float(x), float(y)) * scene.shadowParams.y * penumbra * 0.5;
            sum += litFromDepth(referenceDepth, uv + offset);
        }
    }
    return sum / 25.0;
}

// 遮挡物搜索：在搜索半径内统计落在接收点之前的纹素，返回它们的平均深度。
// 一个遮挡物都没有时返回负值，表示这一点完全受光
float searchBlockers(float referenceDepth, vec2 uv, float radius)
{
    float depthSum = 0.0;
    float count = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const vec2 offset = vec2(float(x), float(y)) * scene.shadowParams.y * radius * 0.5;
            const float depth = texture(shadowMap, uv + offset).r;
            if (referenceDepth > depth) {
                depthSum += depth;
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? depthSum / count : -1.0;
}

// 百分比渐近软阴影：遮挡物搜索给出遮挡物的平均深度，接收点到遮挡物的距离决定半影宽度，
// 遮挡物离接收点越远半影越宽，最后在半影宽度上做比较。
// 距离之比在光源正交投影下换算，那个投影里深度沿光线方向线性变化，
// 归一化深度乘上远近平面间距再加上近平面就是光源到表面的距离
float samplePcss(float referenceDepth, vec2 uv)
{
    const float blockerDepth = searchBlockers(referenceDepth, uv, scene.shadowPcss.x);
    if (blockerDepth < 0.0) {
        return 1.0;
    }

    const float ratio =
        (referenceDepth - blockerDepth) / max(blockerDepth + scene.shadowOptions.w, 1e-5);
    const float penumbra =
        clamp(ratio * scene.shadowPcss.y, scene.shadowPcss.z, scene.shadowPcss.w);
    return filterPenumbra(referenceDepth, uv, penumbra);
}

// 区域内深度的矩：金字塔里每一级的纹素存的是 2^lod 见方小块内深度的一阶与二阶矩，
// 取边长最接近 regionTexels 的那一级，两级之间按距离插值。
// 区域是方的，一阶矩与二阶矩都是块内平均值，块的边长越大越模糊
vec2 fetchRegionMoments(vec2 uv, float regionTexels)
{
    // 一级的纹素是 2^lod 见方小块的均值，采样时还在四个相邻纹素之间插值，
    // 插值把核又摊宽一倍，所以取比区域边长小一级的那一级
    const float maxLevel = -log2(scene.shadowParams.y);
    const float lod = clamp(log2(max(regionTexels, 1.0)) - 1.0, 0.0, maxLevel);
    return textureLod(shadowMap, uv, lod).rg;
}

// 方差软阴影：遮挡物的平均深度与受光比例都由区域内的矩估算，两次区域查询各一次采样。
// 矩只给出一阶与二阶两个量，分布的形状由切比雪夫不等式与一条混合方程补足
float sampleVssm(float referenceDepth, vec2 uv)
{
    // 第一步：用搜索范围的矩估遮挡物的平均深度。
    // 区域的平均深度不小于接收点深度时切比雪夫不等式不成立，按没有遮挡物处理
    const vec2 searchMoments = fetchRegionMoments(uv, scene.shadowPcss.x * 2.0);
    const float searchMean = searchMoments.x;
    const float searchVariance = max(searchMoments.y - searchMean * searchMean, 1e-8);
    const float delta = referenceDepth - searchMean;
    if (delta <= 0.0) {
        return 1.0;
    }

    // 切比雪夫不等式给出区域内深度不小于接收点深度的比例的上界，也就是受光比例的上界
    const float litFraction = searchVariance / (searchVariance + delta * delta);
    const float occludedFraction = 1.0 - litFraction;
    if (occludedFraction < 1e-3) {
        return 1.0;
    }

    // 区域的平均深度是未遮挡与遮挡两部分按比例混合的结果，未遮挡部分的深度近似取接收点深度，
    // 由这个混合关系反解出遮挡部分的平均深度
    const float blockerDepth = (searchMean - litFraction * referenceDepth) / occludedFraction;
    if (blockerDepth <= 0.0) {
        return 1.0;
    }

    const float ratio =
        (referenceDepth - blockerDepth) / max(blockerDepth + scene.shadowOptions.w, 1e-5);
    const float penumbra =
        clamp(ratio * scene.shadowPcss.y, scene.shadowPcss.z, scene.shadowPcss.w);

    // 第二步：在半影范围上再取一次矩，受光比例就是切比雪夫不等式给出的上界
    const vec2 filterMoments = fetchRegionMoments(uv, penumbra * 2.0);
    const float filterMean = filterMoments.x;
    const float filterVariance = max(filterMoments.y - filterMean * filterMean, 1e-8);
    const float filterDelta = referenceDepth - filterMean;
    if (filterDelta <= 0.0) {
        return 1.0;
    }
    return filterVariance / (filterVariance + filterDelta * filterDelta);
}

// 返回 0 到 1 的受光比例：1 表示完全受光，0 表示完全处在阴影里。
// 阴影模式为零时恒为 1
float sampleShadow(vec3 worldPosition, vec3 normal, float nDotL)
{
    const int mode = int(scene.shadowParams.w + 0.5);
    if (mode <= 0) {
        return 1.0;
    }

    // 沿法线方向抬升采样位置，抬升距离是世界空间里一个阴影贴图纹素的宽度；开关关闭时不抬升
    vec3 liftedPosition = worldPosition + normal * (scene.shadowOptions.z * scene.shadowOptions.x);
    bool inside = false;
    vec3 projected = shadowProjection(liftedPosition, inside);
    if (!inside) {
        // 投影落在阴影贴图之外时按受光处理，避免边缘出现整片黑
        return 1.0;
    }

    // 掠射角下深度误差更大，开关打开时偏移随入射角增大
    float bias = scene.shadowParams.x;
    if (scene.shadowOptions.y > 0.5) {
        bias = max(bias * (1.0 - nDotL), bias);
    }
    float referenceDepth = projected.z - bias;

    if (mode >= 3) {
        return sampleVssm(referenceDepth, projected.xy);
    }
    if (mode >= 2) {
        return samplePcss(referenceDepth, projected.xy);
    }
    return filterShadow(referenceDepth, projected.xy, scene.shadowParams.z);
}

#endif
