#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 噪声种类
enum NoiseKind {
    NOISE_VALUE = 0,      // 值噪声：格点上是随机数，格内做插值
    NOISE_PERLIN = 1,     // 梯度噪声：格点上是随机梯度，取梯度与距离的点积
    NOISE_WORLEY = 2,     // 细胞噪声：取到最近特征点的距离
    NOISE_FBM = 3,        // 梯度噪声按倍频叠加
    NOISE_REFERENCE = 4,  // 超采样参考
};

// 与着色器中的 NoiseBuffer 逐字节对应
struct NoiseUniform {
    glm::vec4 params;   // x 噪声种类, y 频率, z 倍频数, w 持续度
    glm::vec4 misc;     // x 间隙度, y 时间, z 参考超采样数（每边的平方根）, w 显示缩放
};

struct FrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet noiseSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct NoiseRenderer {
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout noiseSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline noisePipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    FrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordNoiseMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

const char* noiseKindName(uint32_t kind);
// 每像素的噪声求值次数：基础噪声是 1，多倍频是每个倍频一次，参考是超采样数乘倍频数
uint32_t noiseEvaluationCount(uint32_t kind, uint32_t octaves, uint32_t referenceSide);

void createRenderer(const VulkanContext& ctx, NoiseRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, NoiseRenderer& renderer);

void recreateSwapchainTargets(const VulkanContext& ctx, NoiseRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, NoiseRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const NoiseUniform& uniform, FrameStatistics& outStatistics);
