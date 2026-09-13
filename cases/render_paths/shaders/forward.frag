#version 450

// 前向路径与分块前向路径的着色：每个片元把所有光源或者本块的光源列表过一遍

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inPosition;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, z 分块尺寸, w 每块的光源上限
    vec4 modeParams;       // x 路径, y 光源数量, z 是否用分块光源列表, w 时间
    vec4 miscParams;       // x 环境光强度, yzw 保留
} scene;

layout(set = 0, binding = 2) readonly buffer LightBuffer {
    vec4 lights[];  // 每个光源两个 vec4：position.xyz + 半径, color.rgb + 强度
} lightBuffer;

layout(set = 0, binding = 3) readonly buffer TileBuffer {
    uvec4 tiles[];  // 每块一个 vec4：x 光源数, yzw 前三个光源编号
} tileBuffer;

layout(set = 0, binding = 4) readonly buffer TileLightBuffer {
    uint indices[];  // 每块的光源编号，按每块上限对齐
} tileLights;

vec3 shadeLight(vec3 position, vec3 normal, vec3 albedo, vec3 lightPosition, float radius,
                vec3 lightColor, float intensity)
{
    const vec3 toLight = lightPosition - position;
    const float distance = length(toLight);
    if (distance > radius) {
        return vec3(0.0);
    }
    const float attenuation = clamp(1.0 - distance / radius, 0.0, 1.0);
    return albedo * lightColor * intensity * attenuation * attenuation *
           max(dot(normal, normalize(toLight)), 0.0);
}

void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 albedo = inColor;
    vec3 result = albedo * scene.miscParams.x;

    const uint lightCount = uint(scene.modeParams.y);
    if (scene.modeParams.z < 0.5) {
        // 前向：每个片元把所有光源都过一遍
        for (uint i = 0u; i < lightCount; ++i) {
            const vec4 lightPosition = lightBuffer.lights[i * 2];
            const vec4 lightColor = lightBuffer.lights[i * 2 + 1];
            result += shadeLight(inPosition, normal, albedo, lightPosition.xyz, lightPosition.w,
                                 lightColor.rgb, lightColor.a);
        }
    } else {
        // 分块前向：只过本块列表里的光源
        const uint tileSize = uint(scene.viewportParams.z);
        const uint tilesPerRow = (uint(scene.viewportParams.x) + tileSize - 1u) / tileSize;
        const uvec2 tile = uvec2(gl_FragCoord.xy) / tileSize;
        const uint tileIndex = tile.y * tilesPerRow + tile.x;
        const uint count = tileBuffer.tiles[tileIndex].x;
        const uint maxPerTile = uint(scene.viewportParams.w);

        for (uint i = 0u; i < count; ++i) {
            // 前三个编号打包在 vec4 的 yzw 里，其余的在后面的缓冲里
            uint lightIndex;
            if (i < 3u) {
                lightIndex = tileBuffer.tiles[tileIndex][i + 1u];
            } else {
                lightIndex = tileLights.indices[tileIndex * maxPerTile + i];
            }
            const vec4 lightPosition = lightBuffer.lights[lightIndex * 2];
            const vec4 lightColor = lightBuffer.lights[lightIndex * 2 + 1];
            result += shadeLight(inPosition, normal, albedo, lightPosition.xyz, lightPosition.w,
                                 lightColor.rgb, lightColor.a);
        }
    }

    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
