#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 全分辨率尺寸, zw 低分辨率尺寸
    vec4 modeParams;       // x 比例, y 上采样方式, z 深度获取方式, w 双边强度
    vec4 miscParams;       // x 时间, y 光斑强度, zw 保留
} scene;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;
layout(set = 0, binding = 4) uniform sampler2D sceneColor;
layout(set = 0, binding = 5) uniform sampler2D lowGeometry;
layout(set = 0, binding = 6) uniform sampler2D glowColor;

// 上采样方式
const float UPSAMPLE_BILINEAR = 0.0;
const float UPSAMPLE_BILATERAL = 1.0;

// 深度获取方式
const float DEPTH_RECONSTRUCT = 2.0;

// 从投影后的深度还原视空间深度，双边加权在视空间深度上比较才有意义
float viewDepth(float ndcDepth)
{
    const float near = scene.miscParams.z;
    const float far = scene.miscParams.w;
    return near / max(1.0 - ndcDepth * (1.0 - near / far), 1e-5);
}

void main()
{
    vec3 result = texture(sceneColor, inUv).rgb + vec3(0.0);
    float fullDepth = texture(sceneDepth, inUv).r;
    vec3 fullNormal = normalize(texture(sceneNormal, inUv).xyz);

    vec3 glow;
    if (scene.modeParams.y < 0.5) {
        // 双线性：直接用硬件的线性过滤把低分辨率结果取回
        glow = texture(glowColor, inUv).rgb;
    } else {
        // 联合双边上采样：低分辨率邻域按深度与法线的接近程度加权，跨物体的邻域权重接近零
        const vec2 lowTexel = 1.0 / vec2(textureSize(glowColor, 0));
        const bool reconstruct = scene.modeParams.z >= DEPTH_RECONSTRUCT;
        const float fullViewDepth = viewDepth(fullDepth);
        vec3 sum = vec3(0.0);
        float weightSum = 0.0;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                const vec2 offset = vec2(float(x), float(y)) * lowTexel;
                const vec2 sampleUv = inUv + offset;

                float sampleDepth;
                vec3 sampleNormal;
                if (reconstruct) {
                    // 从全分辨率的深度与法线重新取：先把坐标对齐到低分辨率纹素的中心，
                    // 取到的才是产生那个低分辨率颜色的同一处几何
                    const vec2 lowResSize = vec2(textureSize(glowColor, 0));
                    const vec2 texelCenterUv = (floor(sampleUv * lowResSize) + vec2(0.5)) / lowResSize;
                    sampleDepth = texture(sceneDepth, texelCenterUv).r;
                    sampleNormal = texture(sceneNormal, texelCenterUv).xyz;
                } else {
                    // 拷贝或子通道写出的低分辨率几何
                    const vec4 geometry = texture(lowGeometry, sampleUv);
                    sampleDepth = geometry.x;
                    sampleNormal = geometry.yzw;
                }

                const float depthWeight =
                    exp(-abs(viewDepth(sampleDepth) - fullViewDepth) * scene.modeParams.w * 0.005);
                const float normalWeight = max(dot(normalize(sampleNormal), fullNormal), 0.0);
                const float spatial = exp(-dot(offset, offset) / (lowTexel.x * lowTexel.x * 4.0));
                const float weight = depthWeight * normalWeight * spatial;

                sum += texture(glowColor, sampleUv).rgb * weight;
                weightSum += weight;
            }
        }
        glow = weightSum > 0.0 ? sum / weightSum : texture(glowColor, inUv).rgb;
    }

    result += glow;
    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
