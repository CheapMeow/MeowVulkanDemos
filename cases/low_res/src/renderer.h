#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 低分辨率层的比例
enum LowResRatio {
    LOWRES_RATIO_FULL = 0,      // 全分辨率
    LOWRES_RATIO_HALF = 1,      // 二分之一
    LOWRES_RATIO_QUARTER = 2,   // 四分之一
};

// 低分辨率层回合成全分辨率时的上采样方式
enum LowResUpsample {
    LOWRES_UPSAMPLE_BILINEAR = 0,    // 按硬件线性过滤取回
    LOWRES_UPSAMPLE_BILATERAL = 1,   // 用全分辨率的深度与法线加权
};

// 低分辨率层的深度与法线的获取方式
enum LowResDepthMode {
    LOWRES_DEPTH_COPY = 0,        // 单独一趟降采样，写进一张低分辨率颜色附件
    LOWRES_DEPTH_SUBPASS = 1,     // 与光斑在同一个子通道里写出
    LOWRES_DEPTH_RECONSTRUCT = 2, // 合成时直接从全分辨率的深度与法线重新取
};

// 与着色器中的 SceneBuffer 逐字节对应
struct LowResSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 全分辨率尺寸, zw 低分辨率尺寸
    glm::vec4 modeParams;       // x 比例, y 上采样方式, z 深度获取方式, w 双边强度
    glm::vec4 miscParams;       // x 时间, y 光斑强度, zw 保留
};

// 一帧的绘制选项
struct LowResOptions {
    uint32_t ratio;
    uint32_t upsample;
    uint32_t depthMode;
    float bilateralStrength;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    LowResOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordLowResMilliseconds;
    double cpuRecordCompositeMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t lowResWidth;
    uint32_t lowResHeight;
};

struct LowResFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct LowResRenderer {
    GpuBuffer instanceBuffer;
    uint32_t instanceCount;

    // 全分辨率几何缓冲
    GpuTexture sceneColor;
    GpuTexture sceneNormal;
    GpuTexture sceneDepth;
    // 低分辨率层
    GpuTexture lowGeometry;
    GpuTexture glowColor;

    VkFramebuffer sceneFramebuffer;
    VkFramebuffer downgradeFramebuffer;
    VkFramebuffer glowFramebuffer;
    VkFramebuffer glowGeometryFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass downgradeRenderPass;
    VkRenderPass glowRenderPass;
    VkRenderPass glowGeometryRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipeline downgradePipeline;
    VkPipeline glowPipeline;
    VkPipeline glowGeometryPipeline;
    VkPipelineLayout compositePipelineLayout;
    VkPipeline compositePipeline;

    VkSampler linearSampler;
    VkSampler nearestSampler;

    uint32_t lowResWidth;
    uint32_t lowResHeight;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    LowResFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* lowResRatioName(uint32_t ratio);
const char* lowResUpsampleName(uint32_t upsample);
const char* lowResDepthModeName(uint32_t depthMode);

void createRenderer(const VulkanContext& ctx, LowResRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, LowResRenderer& renderer);

// 交换链重建之后调用。全分辨率与低分辨率附件都跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, LowResRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 比例变化时，函数在本帧开头重建低分辨率附件
bool drawFrame(const VulkanContext& ctx, LowResRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const LowResSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
