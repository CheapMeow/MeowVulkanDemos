#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 内核类型
enum ThroughputKernel {
    THROUGHPUT_ALU = 0,       // 算力密集：一串有数据依赖的三角函数与小数取余
    THROUGHPUT_SAMPLE = 1,    // 采样密集：同一张纹理上取多次
    THROUGHPUT_BRANCH = 2,    // 两条分支，各自算力密集
};

// 默认精度
enum ThroughputPrecision {
    THROUGHPUT_HIGH = 0,      // 高精度
    THROUGHPUT_MEDIUM = 1,    // 中精度
};

// 与着色器中的 ThroughputBuffer 逐字节对应
struct ThroughputUniform {
    glm::vec4 viewportParams;   // xy 全屏尺寸, zw 保留
    glm::vec4 kernelParams;     // x 内核编号, y 迭代次数, z 采样次数, w 采样局部性开关
    glm::vec4 miscParams;       // x 分支发散开关, yzw 保留
};

// 一帧的绘制选项
struct ThroughputOptions {
    uint32_t kernel;
    uint32_t precision;
    uint32_t iterations;
    uint32_t samples;
    bool divergent;
    bool randomLocality;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    ThroughputOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordDispatchMilliseconds;
    double cpuRecordCompositeMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t workgroupCount;
};

struct ThroughputFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet uniformSet;
    VkDescriptorSet resultSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ThroughputRenderer {
    GpuTexture sourceTexture;
    GpuBuffer resultBuffer;

    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout uniformSetLayout;
    VkDescriptorSetLayout resultSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout computePipelineLayout;
    VkPipeline computeHighPipeline;
    VkPipeline computeMediumPipeline;
    VkPipelineLayout displayPipelineLayout;
    VkPipeline displayPipeline;

    VkSampler sampler;

    uint32_t resultWidth;
    uint32_t resultHeight;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ThroughputFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* throughputKernelName(uint32_t kernel);
const char* throughputPrecisionName(uint32_t precision);

void createRenderer(const VulkanContext& ctx, ThroughputRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, ThroughputRenderer& renderer);

// 交换链重建之后调用。结果缓冲与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ThroughputRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, ThroughputRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ThroughputUniform& uniform,
               FrameStatistics& outStatistics);
