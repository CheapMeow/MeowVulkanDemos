#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

// 实例列表与金字塔由相邻帧共用，这里只保留一帧在飞
enum { MAX_FRAMES_IN_FLIGHT = 1 };

// 金字塔层级数：第一层是全分辨率的八分之一，第二层是第一层的八分之一
enum { HZB_LEVEL_COUNT = 2 };

// 有序压缩用一个线程组处理 1024 乘 20 个元素，实例数量不能超过这个上限
enum { SCAN_CAPACITY = 1024 * 20 };

// 时间戳查询：深度预通道、遮挡剔除、金字塔构建、场景与输出各占一对
enum {
    QUERY_PREPASS_BEGIN = 0,
    QUERY_PREPASS_END = 1,
    QUERY_CULL_BEGIN = 2,
    QUERY_CULL_END = 3,
    QUERY_PYRAMID_BEGIN = 4,
    QUERY_PYRAMID_END = 5,
    QUERY_SCENE_BEGIN = 6,
    QUERY_SCENE_END = 7,
    QUERY_COUNT = 8
};

// 与着色器中的 SceneBuffer 逐字节对应
struct HzbSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, z 包围盒扩大系数, w 层级模式
    glm::vec4 modeParams;       // x 是否启用遮挡剔除, y 金字塔取向, z 固定层级, w 时间
    glm::vec4 miscParams;       // x 实例数量, y 是否可视化金字塔, z 可见数量, w 深度来源
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
    double cpuRecordPrepassMilliseconds;
    double cpuRecordCullMilliseconds;
    double cpuRecordPyramidMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuPrepassMilliseconds;
    double gpuCullMilliseconds;
    double gpuPyramidMilliseconds;
    double gpuSceneMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t visibleInstanceCount;
    uint32_t culledInstanceCount;
};

struct HzbFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet cullSet;
    VkDescriptorSet presentSet;
    VkDescriptorSet cullResourceSet;
    VkDescriptorSet pyramidSets[HZB_LEVEL_COUNT];

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct HzbRenderer {
    GpuBuffer instanceBuffer;
    GpuBuffer indexBuffer;
    GpuBuffer visibleBuffer;
    GpuBuffer visibleFlagBuffer;
    GpuBuffer indirectBuffer;
    uint32_t instanceCount;

    GpuTexture colorTexture;
    GpuTexture depthTexture;
    GpuTexture hzbLevels[HZB_LEVEL_COUNT];

    VkFramebuffer depthFramebuffer;
    VkFramebuffer sceneClearFramebuffer;
    VkFramebuffer sceneLoadFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass depthRenderPass;
    VkRenderPass sceneClearRenderPass;
    VkRenderPass sceneLoadRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout textureSetLayout;
    VkDescriptorSetLayout resourceSetLayout;
    VkDescriptorSetLayout hzbSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline boxPipeline;
    VkPipeline depthPipeline;
    VkPipelineLayout presentPipelineLayout;
    VkPipeline presentPipeline;
    VkPipelineLayout cullPipelineLayout;
    VkPipeline cullPipeline;
    VkPipeline scanPipeline;
    VkPipelineLayout hzbPipelineLayout;
    VkPipeline hzbPipeline;

    VkDescriptorSet hzbSets[HZB_LEVEL_COUNT];

    VkSampler linearSampler;
    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    HzbFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* hzbExtremeName(uint32_t extreme);
const char* hzbLevelModeName(uint32_t levelMode);
const char* hzbDepthSourceName(bool currentFrameDepth);

void createRenderer(const VulkanContext& ctx, HzbRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, HzbRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, HzbRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, HzbRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const HzbSceneUniform& uniform, FrameStatistics& outStatistics);
