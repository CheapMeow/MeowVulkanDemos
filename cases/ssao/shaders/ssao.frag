#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outOcclusion;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;    // x 遮蔽量乘到哪一档光照, y 采样数, z 采样半径, w 核大小
    vec4 miscParams;    // x 法线加权开关, y 遮蔽强度, z 近平面, w 远平面
} scene;

layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;
layout(set = 0, binding = 4) uniform sampler2D noiseTexture;

vec3 hash33(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)), dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453);
}

// 从投影后的深度还原视空间位置。相机在原点朝向 -z，投影矩阵是标准的右手透视
vec3 viewPosition(vec2 uv, float ndcDepth)
{
    const float near = scene.miscParams.z;
    const float far = scene.miscParams.w;
    const vec2 ndc = uv * 2.0 - 1.0;
    const float aspect = scene.viewportParams.x / scene.viewportParams.y;

    // 投影矩阵的纵向缩放取 60 度视场角对应的值，与场景通道用的是同一个常数
    const float tanHalfFov = 0.5773503;
    const float viewZ = near * far / max(far - ndcDepth * (far - near), 1e-5);
    return vec3(ndc.x * tanHalfFov * aspect * viewZ, ndc.y * tanHalfFov * viewZ, -viewZ);
}

// 屏幕空间环境光遮蔽：在法线半球上取一圈采样点，比较它们与深度缓冲给出的表面的关系
void main()
{
    const float ndcDepth = texture(sceneDepth, inUv).r;
    if (ndcDepth >= 1.0) {
        // 背景上没有几何，遮蔽量留零
        outOcclusion = vec4(0.0);
        return;
    }

    const vec3 position = viewPosition(inUv, ndcDepth);
    const vec3 normal = normalize(texture(sceneNormal, inUv).xyz);

    // 每个像素用噪声偏移核的起始角度，避免出现规则的条带
    const float noise = texture(noiseTexture, gl_FragCoord.xy / 8.0).r;
    const float angle = noise * 6.2831853;
    const float s = sin(angle);
    const float c = cos(angle);

    // 以法线为 z 轴建一组切空间基
    const vec3 tangent = normalize(abs(normal.z) < 0.99 ? cross(vec3(0.0, 0.0, 1.0), normal)
                                                        : cross(vec3(1.0, 0.0, 0.0), normal));
    const vec3 bitangent = cross(normal, tangent);

    const uint samples = uint(max(scene.modeParams.y, 1.0));
    const float radius = scene.modeParams.z;
    float occlusion = 0.0;

    for (uint i = 0u; i < samples; ++i) {
        const float fi = float(i);
        // 半球上的采样点：方向均匀铺开，半径用平方根分布让它靠近中心一些
        const float theta = (fi + 0.5) / float(samples) * 6.2831853;
        const float localRadius = radius * sqrt((fi + 0.7) / float(samples));
        const vec3 direction = tangent * (cos(theta) * s - sin(theta) * c) * localRadius +
                               bitangent * (sin(theta) * s + cos(theta) * c) * localRadius +
                               normal * localRadius;

        const vec3 samplePosition = position + direction * scene.modeParams.w;
        const vec4 clip = scene.viewProjection * vec4(samplePosition, 1.0);
        if (clip.w <= 0.0) {
            continue;
        }
        const vec2 sampleUv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) {
            // 屏幕外没有信息，按不遮蔽处理
            continue;
        }

        const float sampleDepth = texture(sceneDepth, sampleUv).r;
        const vec3 sampleSurface = viewPosition(sampleUv, sampleDepth);
        // 采样点在深度缓冲给出的表面之后才算被挡住
        const float difference = sampleSurface.z - samplePosition.z;
        if (difference > 0.02) {
            float weight = 1.0;
            if (scene.miscParams.x > 0.5) {
                // 法线加权：两侧法线差别大的地方不算遮蔽，可以压掉一些自遮蔽噪点
                const vec3 sampleNormal = normalize(texture(sceneNormal, sampleUv).xyz);
                weight = max(dot(normal, sampleNormal), 0.0);
                weight = pow(weight, 2.0);
            }
            occlusion += weight;
        }
    }

    float value = 1.0 - clamp(occlusion / float(samples) * scene.miscParams.y, 0.0, 1.0);
    outOcclusion = vec4(value, 0.0, 0.0, 1.0);
}
