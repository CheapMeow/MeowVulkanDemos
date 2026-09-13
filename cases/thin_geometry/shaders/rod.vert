#version 450

layout(location = 0) out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 colorParams;      // rgb 背景亮度, a 保留
    vec4 modeParams;       // x 平移的像素数, yzw 保留
    vec4 params;           // x 细杆基准宽度（像素）, yzw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer RodBuffer {
    vec4 rods[];  // xy 中心, z 宽度倍数, w 亮度
} rodBuffer;

void main()
{
    // 四个顶点拼成一根竖直细杆，角点由顶点编号推出，不需要顶点缓冲
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    vec4 rod = rodBuffer.rods[gl_InstanceIndex];
    float pixelToNdc = 2.0 / scene.viewportParams.x;
    float widthNdc = scene.params.x * rod.z * pixelToNdc;
    float centerX = rod.x + scene.modeParams.x * pixelToNdc;

    float x = centerX + (corner.x - 0.5) * widthNdc;
    float y = mix(-0.9, 0.9, corner.y);

    gl_Position = vec4(x, y, 0.0, 1.0);
    outColor = vec3(rod.w);
}
