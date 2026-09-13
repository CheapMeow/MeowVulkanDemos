#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 降采样链最多几级
enum { BLOOM_MAX_LEVELS = 5 };

// 亮部取出的方式
enum BloomThreshold {
    BLOOM_THRESHOLD_NONE = 0,   // 不做泛光
    BLOOM_THRESHOLD_HARD = 1,   // 硬阈值
    BLOOM_THRESHOLD_SOFT = 2,   // 软阈值
};

// 降采样链的实现
enum BloomChain {
    BLOOM_CHAIN_GAUSSIAN = 0,   // 逐级高斯
    BLOOM_CHAIN_KAWASE = 1,     // 一降一升的 Kawase 方式
    BLOOM_CHAIN_MULTI = 2,      // 一次采样多个层级
};

// 抗锯齿与色调映射的先后
enum BloomOrder {
    BLOOM_ORDER_AA_FIRST = 0,   // 先抗锯齿再色调映射
    BLOOM_ORDER_TONEMAP_FIRST = 1,  // 先色调映射再抗锯齿
};

// 与着色器中的 BloomBuffer 逐字节对应
struct BloomSceneUniform {
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 modeParams;       // x 阈值模式, y 链实现, z 是否应用阈值, w 抗锯齿顺序
    glm::vec4 miscParams;       // x 阈值, y 泛光强度, z 时间, w 保留
};

// 一帧的绘制选项
struct BloomOptions {
    uint32_t threshold;
    uint32_t chain;
    uint32_t order;
    uint32_t levels;
    float thresholdValue;
    float intensity;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    BloomOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordChainMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

struct BloomFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet thresholdSet;
    VkDescriptorSet downSets[BLOOM_MAX_LEVELS];
    VkDescriptorSet upSets[BLOOM_MAX_LEVELS];
    VkDescriptorSet combineSet;
    VkDescriptorSet presentSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct BloomRenderer {
    GpuTexture hdrScene;
    GpuTexture levels[BLOOM_MAX_LEVELS];
    GpuTexture bloomResult;
    GpuTexture blackTexture;

    VkFramebuffer sceneFramebuffer;
    VkFramebuffer levelFramebuffers[BLOOM_MAX_LEVELS];
    VkFramebuffer combineFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass writeRenderPass;
    VkRenderPass blendRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout textureSetLayout;
    VkDescriptorSetLayout combineSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipeline downsamplePipeline;
    VkPipeline upsamplePipeline;
    VkPipelineLayout combinePipelineLayout;
    VkPipeline combinePipeline;
    VkPipelineLayout presentPipelineLayout;
    VkPipeline presentPipeline;

    VkSampler linearSampler;
    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    BloomFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* bloomThresholdName(uint32_t threshold);
const char* bloomChainName(uint32_t chain);
const char* bloomOrderName(uint32_t order);

void createRenderer(const VulkanContext& ctx, BloomRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, BloomRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, BloomRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, BloomRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const BloomSceneUniform& uniform, FrameStatistics& outStatistics);
