#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

// 离屏附件、每像素的链表头与节点池都由相邻帧共用，两帧同时在飞会互相踩踏，
// 因此这里只保留一帧在飞：每帧开头等上一帧的栅栏，读回的节点数也才是确定的一帧的结果
enum { MAX_FRAMES_IN_FLIGHT = 1 };

// 层数上限
enum { TRANSPARENCY_MAX_LAYERS = 12 };

// 逐像素链表的节点池容量与单像素最多解析的层数
enum { TRANSPARENCY_NODE_CAPACITY = 1 << 22 };
enum { TRANSPARENCY_MAX_LIST = 16 };

// 混合方式
enum TransparencyMode {
    TRANSPARENCY_FAR_TO_NEAR = 0,   // 源混合，由远到近
    TRANSPARENCY_NEAR_TO_FAR = 1,   // 源混合，由近到远
    TRANSPARENCY_UNSORTED = 2,      // 源混合，乱序
    TRANSPARENCY_WEIGHTED = 3,      // 加权混合
    TRANSPARENCY_LINKED_LIST = 4,   // 逐像素链表
};

// 与着色器中的 SceneBuffer 逐字节对应
struct TransparencySceneUniform {
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 colorParams;      // rgb 背景亮度, a 保留
    glm::vec4 modeParams;       // x 混合方式, y 是否输出热力图, zw 保留
    glm::vec4 layerParams;      // x 层数, y 节点池容量, zw 保留
};

// 一层色片占用形状缓冲里的两个 vec4：
//   rect  = (中心 x, 中心 y, 边长, 透明度)
//   color = (rgb, 裁剪空间深度)
struct TransparencyLayer {
    glm::vec4 rect;
    glm::vec4 color;
};

// 一帧的绘制选项
struct TransparencyOptions {
    uint32_t mode;
    uint32_t layerCount;
    bool heatmap;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    TransparencyOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordGeometryPassMilliseconds;
    double cpuRecordResolveMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t nodeCount;
    uint32_t nodeOverflow;
    uint64_t listBytes;
};

struct TransparencyFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    GpuBuffer counterReadback;
    VkDescriptorSet sceneSet;
    VkDescriptorSet outputSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct TransparencyRenderer {
    // 每层两个 vec4，缓冲里放三份：由远到近、由近到远、乱序
    GpuBuffer layerBuffer;

    GpuTexture hdrColor;
    GpuTexture accumColor;
    GpuTexture revealage;

    GpuBuffer layerCountBuffer;
    GpuBuffer headBuffer;
    GpuBuffer nodeColorBuffer;
    GpuBuffer nodeMetaBuffer;
    GpuBuffer nodeCounterBuffer;

    VkFramebuffer sourceFramebuffer;
    VkFramebuffer weightedFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sourceRenderPass;
    VkRenderPass weightedRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout outputSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline sourcePipeline;
    VkPipeline weightedPipeline;
    VkPipeline listPipeline;
    VkPipelineLayout outputPipelineLayout;
    VkPipeline compositePipeline;
    VkPipeline weightedResolvePipeline;
    VkPipeline listResolvePipeline;

    VkSampler sampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    TransparencyFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* transparencyModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, TransparencyRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, TransparencyRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, TransparencyRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, TransparencyRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const TransparencySceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
