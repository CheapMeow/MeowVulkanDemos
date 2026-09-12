#version 450

// 逐采样点解析：先编码到感知空间求平均，再解码回线性，
// 与硬件盒式解析在亮度极高的边界处会给出不同的结果
layout(set = 0, binding = 0) uniform sampler2DMS colorSamples;

layout(push_constant) uniform PushConstants {
    int sampleCount;
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        if (i >= push.sampleCount) {
            break;
        }
        vec3 linearColor = texelFetch(colorSamples, pixel, i).rgb;
        sum += pow(clamp(linearColor, 0.0, 1.0), vec3(1.0 / 2.2));
    }

    vec3 averaged = sum / float(push.sampleCount);
    outColor = vec4(pow(averaged, vec3(2.2)), 1.0);
}
