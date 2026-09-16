// 生产者与消费者共用的参数块，与主机侧的 PatternUniform 逐字节对应。
// resolutionAndPhase 的 xy 是图案尺寸（像素），z 是相位，w 是叠加层数
layout(set = 0, binding = 0) uniform PatternUniform
{
    vec4 resolutionAndPhase;
    vec4 extra;
} uniformData;
