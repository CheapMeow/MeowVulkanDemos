#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 延迟管线的两条路径
enum SubpassPath {
    SUBPASS_PATH_RENDER_PASSES = 0,   // 几何与光照分成两个独立渲染通道，中间解析到主存
    SUBPASS_PATH_SUBPASS = 1,         // 合成一个渲染通道的两个子通道，几何缓冲留在片上
};

// 与着色器中的 SceneBuffer 逐字节对应
struct SubpassSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 modeParams;       // x 路径, y 乒乓次数, zw 保留
    glm::vec4 miscParams;       // x 乒乓的亮度步长, yzw 保留
};

// 一帧的绘制选项
struct SubpassOptions {
    uint32_t path;
    uint32_t pingPongCount;
    bool transientGeometry;
    float pingPongStep;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    SubpassOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordGeometryMilliseconds;
    double cpuRecordLightingMilliseconds;
    double cpuRecordPingPongMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

struct SubpassFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet geometrySet;
    VkDescriptorSet lightingSet;
    VkDescriptorSet inputSet;
    VkDescriptorSet pingSets[2];
    VkDescriptorSet presentSet;
    VkDescriptorSet presentPingSets[2];
    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct SubpassRenderer {
    GpuBuffer instanceBuffer;
    uint32_t instanceCount;

    GpuTexture gbufferAlbedo;
    GpuTexture gbufferNormal;
    GpuTexture gbufferDepth;
    GpuTexture litTexture;
    GpuTexture pingPong[2];

    VkFramebuffer geometryFramebuffer;
    VkFramebuffer lightingFramebuffer;
    VkFramebuffer subpassFramebuffer;
    VkFramebuffer pingPongFramebuffer[2];
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass geometryRenderPass;
    VkRenderPass lightingRenderPass;
    VkRenderPass subpassRenderPass;
    VkRenderPass subpassRenderPassStored;
    VkRenderPass pingPongRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout textureSetLayout;
    VkDescriptorSetLayout inputSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorPool inputDescriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline geometryPipeline;
    VkPipeline lightingPipeline;
    VkPipeline subpassGeometryPipeline;
    VkPipeline subpassGeometryStoredPipeline;
    VkPipelineLayout inputPipelineLayout;
    VkPipeline subpassLightingPipeline;
    VkPipeline subpassLightingStoredPipeline;
    VkPipelineLayout effectPipelineLayout;
    VkPipeline pingPongPipeline;
    VkPipeline presentPipeline;

    VkSampler linearSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    SubpassFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* subpassPathName(uint32_t path);

void createRenderer(const VulkanContext& ctx, SubpassRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, SubpassRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, SubpassRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, SubpassRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SubpassSceneUniform& uniform, FrameStatistics& outStatistics);
