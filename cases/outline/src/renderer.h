#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 描边方式
enum OutlineMode {
    OUTLINE_NONE = 0,           // 不描边
    OUTLINE_POST = 1,           // 后处理描边，用深度与法线的不连续找边缘
    OUTLINE_HULL = 2,           // 双 Pass 外扩，正面剔除加沿法线外推
};

// 与着色器中的 SceneBuffer 逐字节对应
struct OutlineSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 modeParams;       // x 描边方式, y 深度通道开关, z 法线通道开关, w 外扩距离
    glm::vec4 outlineParams;    // x 阈值, y 线宽, z 是否按屏幕距离缩放, w 保留
};

// 一个实例：位置与尺寸，颜色与种类
struct OutlineInstance {
    glm::vec4 centerSize;
    glm::vec4 colorKind;
};

// 一帧的绘制选项
struct OutlineOptions {
    uint32_t mode;
    bool depthChannel;
    bool normalChannel;
    float threshold;
    float lineWidth;
    float extrudeDistance;
    float cameraDistance;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    OutlineOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordCompositeMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

struct OutlineFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct OutlineRenderer {
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    GpuBuffer instanceBuffer;
    uint32_t boxInstanceCount;
    uint32_t plateInstanceCount;
    uint32_t boxIndexCount;
    uint32_t plateIndexCount;
    uint32_t plateIndexOffset;
    uint32_t boxVertexCount;
    uint32_t plateVertexCount;

    GpuTexture sceneColor;
    GpuTexture sceneNormal;
    GpuTexture sceneDepth;

    VkFramebuffer sceneFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline hullPipeline;
    VkPipeline sceneBoxPipeline;
    VkPipeline scenePlatePipeline;
    VkPipelineLayout compositePipelineLayout;
    VkPipeline outlinePipeline;
    VkPipeline presentPipeline;

    VkSampler linearSampler;
    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    OutlineFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* outlineModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, OutlineRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, OutlineRenderer& renderer);

// 交换链重建之后调用。离屏附件跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, OutlineRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, OutlineRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const OutlineSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
