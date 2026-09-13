#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 渲染路径
enum RenderPath {
    RENDER_PATH_FORWARD = 0,   // 前向：每个片元把所有光源过一遍
    RENDER_PATH_DEFERRED = 1,  // 延迟：几何缓冲加屏幕空间光照
    RENDER_PATH_TILED = 2,     // 分块前向：先按屏幕分块建光源列表
};

// 光源数量上限
enum { MAX_LIGHT_COUNT = 1024 };

// 与着色器中的 SceneBuffer 逐字节对应
struct RenderPathSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, z 分块尺寸, w 每块的光源上限
    glm::vec4 modeParams;       // x 路径, y 光源数量, z 是否用分块列表, w 时间
    glm::vec4 miscParams;       // x 环境光强度, yzw 保留
};

// 一个光源：位置与半径，颜色与强度
struct RenderPathLight {
    glm::vec4 positionRadius;
    glm::vec4 colorIntensity;
};

// 一帧的绘制选项
struct RenderPathOptions {
    uint32_t path;
    uint32_t lightCount;
    uint32_t tileSize;
    uint32_t maxLightsPerTile;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    RenderPathOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordTileMilliseconds;
    double cpuRecordGeometryMilliseconds;
    double cpuRecordLightingMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t tileCount;
};

struct RenderPathFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet tileSet;
    VkDescriptorSet presentForwardSet;
    VkDescriptorSet presentDeferredSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct RenderPathRenderer {
    GpuBuffer instanceBuffer;
    uint32_t instanceCount;
    GpuBuffer lightBuffer;

    GpuBuffer tileBuffer;
    GpuBuffer tileLightBuffer;
    uint32_t tileCapacity;

    GpuTexture forwardColor;
    GpuTexture gbufferAlbedo;
    GpuTexture gbufferNormal;
    GpuTexture gbufferPosition;
    GpuTexture depthTexture;
    GpuTexture litTexture;

    VkFramebuffer forwardFramebuffer;
    VkFramebuffer gbufferFramebuffer;
    VkFramebuffer litFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass forwardRenderPass;
    VkRenderPass gbufferRenderPass;
    VkRenderPass litRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout tileSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline forwardPipeline;
    VkPipeline gbufferPipeline;
    VkPipelineLayout lightingPipelineLayout;
    VkPipeline deferredLightingPipeline;
    VkPipelineLayout presentPipelineLayout;
    VkPipeline presentPipeline;
    VkPipelineLayout tilePipelineLayout;
    VkPipeline tilePipeline;

    VkSampler linearSampler;
    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    RenderPathFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* renderPathName(uint32_t path);

void createRenderer(const VulkanContext& ctx, RenderPathRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, RenderPathRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, RenderPathRenderer& renderer);

// 每帧更新光源的位置与颜色
void updateLights(RenderPathRenderer& renderer, uint32_t lightCount, float time);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, RenderPathRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const RenderPathSceneUniform& uniform,
               FrameStatistics& outStatistics);
