// 阴影采样。调用方必须先声明 shadowMap 与 scene
#ifndef SHADOW_SAMPLING_GLSL
#define SHADOW_SAMPLING_GLSL

// 阴影贴图把深度值存在颜色附件里，比较在着色器里手动完成：
// 参考深度不大于贴图里存的深度就算受光，否则处在阴影里
float litFromDepth(float referenceDepth, vec2 uv)
{
    return referenceDepth <= texture(shadowMap, uv).r ? 1.0 : 0.0;
}

// 返回 0 到 1 的受光比例：1 表示完全受光，0 表示完全处在阴影里。
// 阴影开关关闭时恒为 1
float sampleShadow(vec3 worldPosition, vec3 normal, float nDotL)
{
    if (scene.shadowParams.w < 0.5) {
        return 1.0;
    }

    // 沿法线方向抬升采样位置，减少自阴影产生的条纹
    vec3 liftedPosition = worldPosition + normal * scene.shadowParams.x * 2.0;
    bool inside = false;
    vec3 projected = shadowProjection(liftedPosition, inside);
    if (!inside) {
        // 投影落在阴影贴图之外时按受光处理，避免边缘出现整片黑
        return 1.0;
    }

    // 掠射角下深度误差更大，偏移随入射角增大
    float bias = max(scene.shadowParams.x * (1.0 - nDotL), scene.shadowParams.x);
    float referenceDepth = projected.z - bias;
    float radius = scene.shadowParams.z;

    if (radius <= 0.0) {
        return litFromDepth(referenceDepth, projected.xy);
    }

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * scene.shadowParams.y * radius;
            sum += litFromDepth(referenceDepth, projected.xy + offset);
        }
    }
    return sum / 9.0;
}

#endif
