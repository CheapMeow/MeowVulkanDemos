// 着色吞吐微基准的内核主体。两个精度版本各自声明默认精度之后包含本文件。
// 全屏的计算着色器按界面参数在几组内核之间切换，结果写进存储缓冲以免被优化掉

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform ThroughputBuffer {
    vec4 viewportParams;   // xy 全屏尺寸, zw 保留
    vec4 kernelParams;     // x 内核编号, y 迭代次数, z 采样次数, w 采样局部性开关
    vec4 miscParams;       // x 分支发散开关, yzw 保留
} throughput;

layout(set = 0, binding = 1) uniform sampler2D sourceTexture;

layout(set = 1, binding = 0) buffer ResultBuffer {
    float values[];
} result;

// 内核编号
const uint KERNEL_ALU = 0;        // 算力密集
const uint KERNEL_SAMPLE = 1;     // 采样密集
const uint KERNEL_BRANCH = 2;     // 两条分支，各自算力密集

vec3 hash33(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)), dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453);
}

// 一段有数据依赖的浮点计算，编译器无法把它化简掉
float aluWork(float value, uint iterations)
{
    float accumulator = value;
    for (uint i = 0u; i < iterations; ++i) {
        accumulator = sin(accumulator * 1.0001 + 0.37) + cos(accumulator * 0.9997);
        accumulator = fract(accumulator * 1.6180339);
    }
    return accumulator;
}

float sampleWork(vec2 uv, uint samples)
{
    float accumulator = 0.0;
    for (uint i = 0u; i < samples; ++i) {
        const float offset = float(i);
        vec2 coordinate;
        if (throughput.kernelParams.w > 0.5) {
            // 采样局部性差：每次都跳到纹理上的随机位置
            const vec3 randomness = hash33(vec3(uv * 1024.0, offset));
            coordinate = vec2(randomness.x, randomness.y);
        } else {
            // 采样局部性好：只在附近取
            coordinate = uv + vec2(offset * 0.0015, offset * 0.0009);
        }
        accumulator += texture(sourceTexture, coordinate).r;
    }
    return accumulator / float(samples);
}

void main()
{
    const ivec2 size = ivec2(throughput.viewportParams.xy);
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    const vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
    const uint iterations = uint(throughput.kernelParams.y);
    const uint samples = uint(max(throughput.kernelParams.z, 1.0));
    const uint kernel = uint(throughput.kernelParams.x);

    float value;
    if (kernel == KERNEL_ALU) {
        value = aluWork(uv.x, iterations);
    } else if (kernel == KERNEL_SAMPLE) {
        value = sampleWork(uv, samples);
    } else {
        // 发散时同一个线程组里相邻线程走不同的分支，不发散时整组一起走同一条
        bool takeFirst;
        if (throughput.miscParams.x > 0.5) {
            takeFirst = ((pixel.x + pixel.y) & 1) == 0;
        } else {
            takeFirst = (gl_WorkGroupID.x & 1u) == 0u;
        }
        value = takeFirst ? aluWork(uv.x, iterations) : aluWork(uv.y, iterations + 1u);
    }

    result.values[pixel.y * size.x + pixel.x] = value;
}
