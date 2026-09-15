#version 450
#extension GL_GOOGLE_include_directive : require

#include "environment_common.glsl"

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec2 ndc = inUv * 2.0 - 1.0;
    const vec4 farPoint = sb.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    const vec3 viewDirection = normalize(farPoint.xyz / farPoint.w - sb.cameraPosition.xyz);

    vec3 color;
    if (sb.options.y < BACKGROUND_ORIGINAL - 0.5) {
        color = shRadiance(viewDirection, int(sb.options.x));
    } else {
        color = environmentRadiance(rotateAboutY(viewDirection, -sb.rotationParams.x));
    }

    outColor = vec4(encodeDisplay(color), 1.0);
}
