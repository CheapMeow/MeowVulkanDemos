#version 450

layout(set = 0, binding = 0) uniform sampler2D resolvedColor;

layout(location = 0) out vec4 outColor;

void main()
{
    // 背景亮度高于 1，先做色调映射再编码，否则高光会被直接截断
    vec3 color = texture(resolvedColor, gl_FragCoord.xy / vec2(textureSize(resolvedColor, 0))).rgb;
    color = color / (color + vec3(1.0));
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
