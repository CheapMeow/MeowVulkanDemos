// 程序生成的天空：渐变底色加几个亮斑，等距圆柱投影下按方向取值。
// 预计算与直接采样都用这一个函数，两者的差别只剩在有没有做卷积

const float PI = 3.14159265359;

vec3 skyColor(vec3 direction)
{
    // 上下渐变
    const float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 color = mix(vec3(0.06, 0.07, 0.09), vec3(0.35, 0.48, 0.72), height);

    // 地面偏暗
    if (direction.y < 0.0) {
        color = mix(color, vec3(0.05, 0.045, 0.04), clamp(-direction.y * 3.0, 0.0, 1.0));
    }

    // 几个方向上的亮斑，用来检验高光的宽窄与位置
    const vec3 spots[3] = vec3[3](normalize(vec3(0.5, 0.6, 0.6)), normalize(vec3(-0.7, 0.25, 0.4)),
                                  normalize(vec3(0.1, -0.4, -0.9)));
    const vec3 tints[3] = vec3[3](vec3(9.0, 7.6, 5.4), vec3(1.2, 2.4, 5.0), vec3(4.0, 1.4, 0.5));
    for (int i = 0; i < 3; ++i) {
        const float alignment = max(dot(normalize(direction), spots[i]), 0.0);
        color += tints[i] * pow(alignment, 220.0);
    }
    return color;
}

// 方向与等距圆柱投影坐标的互换
vec2 directionToUv(vec3 direction)
{
    return vec2(atan(direction.z, direction.x) / (2.0 * PI) + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / PI);
}

vec3 uvToDirection(vec2 uv)
{
    const float phi = (uv.x - 0.5) * 2.0 * PI;
    const float theta = uv.y * PI;
    return vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// 低差异序列，采样积分时用来铺开方向
float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint count)
{
    return vec2(float(i) / float(count), radicalInverse(i));
}

// 以法线为 z 轴的一组切空间基
void basisFromNormal(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
    const vec3 up = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}
