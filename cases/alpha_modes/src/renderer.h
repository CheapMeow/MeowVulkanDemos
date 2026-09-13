#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 可选的采样数
enum { ALPHA_SAMPLE_OPTION_COUNT = 4 };

// 镂空的处理方式
enum AlphaMode {
    ALPHA_MODE_TEST = 0,             // alpha test：低于阈值直接丢弃，边界是硬的
    ALPHA_MODE_BLEND = 1,            // alpha blend：不写深度，按由远到近排序后混合
    ALPHA_MODE_TO_COVERAGE = 2,      // alpha to coverage：按 alpha 对采样点做概率判定
};

// 与着色器中的 SceneBuffer 逐字节对应
struct AlphaSceneUniform {
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 colorParams;      // rgb 背景亮度, a 保留
    glm::vec4 modeParams;       // x 处理方式, y alpha 阈值, zw 保留
    glm::vec4 animationParams;  // x 累计旋转角度, yzw 保留
};

// 一块形状占用形状缓冲里的两个 vec4：
//   rect   = (左下角 x, 左下角 y, 边长, 基准透明度)
//   params = (逻辑深度, 旋转相位, 颜色种子, 保留)
struct AlphaShape {
    glm::vec4 rect;
    glm::vec4 params;
};

// 一帧的绘制选项
struct AlphaOptions {
    uint32_t mode;
    uint32_t sampleCount;
    float threshold;
    float accumulatedAngle;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    AlphaOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordGeometryPassMilliseconds;
    double cpuRecordResolveMilliseconds;
    double cpuRecordTonemapMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t fragmentCount;
};

struct AlphaFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    GpuBuffer counterBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct AlphaRenderer {
    // 形状缓冲里存两份：前半按由远到近排序，后半按由近到远排序。每块形状两个 vec4
    GpuBuffer shapeBuffer;
    uint32_t shapeCount;

    GpuTexture msaaColor;
    GpuTexture msaaDepth;
    GpuTexture resolveTexture;

    VkFramebuffer geometryFramebuffer;
    std::vector<VkFramebuffer> tonemapFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass geometryRenderPass;
    VkRenderPass tonemapRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout samplerSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet samplerSet;

    VkPipelineLayout geometryPipelineLayout;
    VkPipeline geometryTestPipeline;
    VkPipeline geometryBlendPipeline;
    VkPipeline geometryCoveragePipeline;
    VkPipelineLayout tonemapPipelineLayout;
    VkPipeline tonemapPipeline;

    VkSampler sampler;

    uint32_t sampleCount;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    AlphaFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const uint32_t* alphaSampleOptions();
const char* alphaModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, AlphaRenderer& renderer, uint32_t sampleCount);
void destroyRenderer(const VulkanContext& ctx, AlphaRenderer& renderer);

// 交换链重建之后调用。多重采样附件与解析目标跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, AlphaRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 采样数变化时，函数在本帧开头重建多重采样附件与全部管线
bool drawFrame(const VulkanContext& ctx, AlphaRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const AlphaSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
