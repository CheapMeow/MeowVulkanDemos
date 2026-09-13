#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 预滤波链的层级数：每一级用一个粗糙度卷积出来
enum { IBL_PREFILTER_LEVELS = 6 };

// 与着色器中的 SceneBuffer 逐字节对应
struct IblSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 modeParams;       // x 是否使用分离求和, y 预滤波贴图的层级数, zw 保留
    glm::vec4 miscParams;       // x 时间, y 是否开启视差矫正, z 包围形状, w 保留
};

// 一帧的绘制选项
struct IblOptions {
    bool splitSum;
    bool parallaxCorrection;
    bool recompute;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    IblOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordPrecomputeMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

struct IblFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet precomputeSet;
    VkDescriptorSet boxSet;
    VkDescriptorSet prefilterSet;
    VkDescriptorSet presentSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct IblRenderer {
    GpuBuffer instanceBuffer;
    GpuBuffer indexBuffer;
    uint32_t instanceCount;

    GpuTexture environment;
    GpuTexture irradiance;
    GpuTexture prefiltered[IBL_PREFILTER_LEVELS];
    GpuTexture brdfLut;
    GpuTexture sceneColor;
    GpuTexture depthTexture;

    VkFramebuffer precomputeFramebuffers[3];   // 环境、辐照度、查找表
    VkFramebuffer prefilteredFramebuffers[IBL_PREFILTER_LEVELS];
    VkFramebuffer sceneFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass precomputeRenderPass;
    VkRenderPass sceneRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout textureSetLayout;
    VkDescriptorSetLayout prefilterSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout precomputePipelineLayout;
    VkPipeline environmentPipeline;
    VkPipeline irradiancePipeline;
    VkPipeline prefilterPipeline;
    VkPipeline brdfPipeline;
    VkPipelineLayout scenePipelineLayout;
    VkPipeline boxPipeline;
    VkPipelineLayout presentPipelineLayout;
    VkPipeline presentPipeline;

    VkSampler linearSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    IblFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

void createRenderer(const VulkanContext& ctx, IblRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, IblRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, IblRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, IblRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const IblSceneUniform& uniform, FrameStatistics& outStatistics);
