#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 屏幕宽度不足一个像素的细杆在后处理抗锯齿与多重采样下的不同结果
enum AntialiasMode {
    ANTIALIAS_NONE = 0,     // 单采样，没有后处理
    ANTIALIAS_MSAA = 1,     // 多重采样附件，硬件解析
    ANTIALIAS_FXAA = 2,     // 单采样光栅化，之后做一遍 FXAA 后处理
};

// 与着色器中的 SceneBuffer 逐字节对应
struct ThinSceneUniform {
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 colorParams;      // rgb 背景亮度, a 保留
    glm::vec4 modeParams;       // x 平移偏移, yzw 保留
    glm::vec4 params;           // x 细杆基准宽度, yzw 保留
};

// 一帧的绘制选项
struct ThinOptions {
    uint32_t mode;
    uint32_t sampleCount;
    uint32_t rodCount;
    uint32_t fxaaSearchSteps;
    float fxaaEdgeThreshold;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    ThinOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordGeometryPassMilliseconds;
    double cpuRecordAntialiasMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

struct ThinFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ThinRenderer {
    GpuBuffer rodBuffer;
    uint32_t rodCount;
    uint32_t rodCapacity;

    // sceneColor 是光栅化的目标，采样数随抗锯齿方式变化；
    // sceneResolved 是多重采样解析之后或单采样直接写入的单采样结果
    GpuTexture sceneColor;
    GpuTexture sceneResolved;

    VkFramebuffer geometryFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass geometryRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout samplerSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet samplerSet;

    VkPipelineLayout geometryPipelineLayout;
    VkPipeline geometryPipeline;
    VkPipelineLayout outputPipelineLayout;
    VkPipeline copyPipeline;
    VkPipeline fxaaPipeline;

    VkSampler sampler;

    uint32_t sampleCount;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ThinFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

void createRenderer(const VulkanContext& ctx, ThinRenderer& renderer, uint32_t sampleCount);
void destroyRenderer(const VulkanContext& ctx, ThinRenderer& renderer);

// 交换链重建之后调用。多重采样附件与解析目标跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ThinRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, ThinRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ThinSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
