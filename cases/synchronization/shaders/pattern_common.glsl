// 图案本身。计算着色器与子通道 0 的光栅化着色器都调用这个函数，两条生产者路径写出的
// 图案因此逐像素相同，不同的同步方式才有可比性。
// 全部计算用 precise 修饰：不加它时编译器可以把乘加融合成一条乘加指令，两条管线的融合
// 结果会差最后一两个最低位。
// 高频结构先化成 0 到 1 的相位再送进 sin，sin 的参数不超过一个圆周，两条管线在参数不大的
// 区间里取值一致。
// 函数直接返回 8 位字节值，图案纹理用整数格式：浮点到定点数的取整方式由实现决定，
// 换成整数之后存储与读取都没有取整这一环
// pixelCenter 是像素中心坐标，resolution 是图案尺寸（像素），phase 是相位，
// layerCount 是叠加层数，用来把生产者的工作量放大到可以测量
uvec4 patternTexel(vec2 pixelCenter, vec2 resolution, float phase, float layerCount)
{
    const float TAU = 6.28318530718;

    // 以图案中心为原点的坐标，短边为 1
    precise vec2 centered = (pixelCenter - 0.5 * resolution) / min(resolution.x, resolution.y);
    precise float radius = length(centered);
    precise float angle = atan(centered.y, centered.x);

    precise float spiralPhase = fract(angle * 5.0 + radius * 17.0 - phase * 3.0);
    precise float ringPhase = fract(radius * 26.0 - phase * 5.0);
    precise float weavePhase = fract((pixelCenter.x + pixelCenter.y) * 0.00067 + phase * 2.0);
    precise float counterPhase = fract((pixelCenter.x - pixelCenter.y) * 0.00091);

    precise float spiral = sin(TAU * spiralPhase);
    precise float rings = sin(TAU * ringPhase);
    precise float weave = sin(TAU * weavePhase) * sin(TAU * counterPhase);

    precise float accumulated = 0.0;
    for (int layer = 0; layer < int(layerCount); ++layer) {
        precise float layerPhase =
            fract(spiralPhase * 0.41 + ringPhase * 0.29 + weavePhase * 0.23 + float(layer) * 0.0617);
        accumulated += 0.5 + 0.5 * sin(TAU * layerPhase);
    }
    accumulated /= max(layerCount, 1.0);

    precise vec3 color = vec3(0.5 + 0.5 * spiral * rings, accumulated, 0.5 + 0.5 * weave * spiral);
    color = clamp(color, 0.0, 1.0);

    return uvec4(uvec3(floor(color * 255.0 + 0.5)), 255u);
}
