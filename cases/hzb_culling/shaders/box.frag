#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    outColor = vec4(pow(inColor * (0.30 + 0.75 * diffuse), vec3(1.0 / 2.2)), 1.0);
}
