#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 遮蔽量乘到哪一档光照
enum SsaoMode {
    SSAO_MODE_OFF = 0,        // 不做遮蔽
    SSAO_MODE_AMBIENT = 1,    // 只乘环境光与间接光
    SSAO_MODE_ALL = 2,        // 乘到全部光照
};

// 与着色器中的 SceneBuffer 逐字节对应
struct SsaoSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, zw 保留
    glm::vec4 modeParams;       // x 遮蔽方式, y 采样数, z 采样半径, w 核大小
    glm::vec4 miscParams;       // x 法线加权开关, y 遮蔽强度, z 近平面, w 远平面
};

// 一帧的绘制选项
struct SsaoOptions {
    uint32_t mode;
    uint32_t samples;
    float radius;
    float kernelSize;
    bool normalWeighting;
    float strength;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    SsaoOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordOcclusionMilliseconds;
    double cpuRecordCompositeMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

struct SsaoFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet occlusionSet;
    VkDescriptorSet compositeSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct SsaoRenderer {
    GpuBuffer instanceBuffer;
    uint32_t instanceCount;

    GpuTexture sceneColor;
    GpuTexture sceneNormal;
    GpuTexture sceneDepth;
    GpuTexture occlusion;
    GpuTexture noiseTexture;

    VkFramebuffer sceneFramebuffer;
    VkFramebuffer occlusionFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass occlusionRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipeline occlusionPipeline;
    VkPipelineLayout compositePipelineLayout;
    VkPipeline compositePipeline;

    VkSampler linearSampler;
    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    SsaoFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* ssaoModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, SsaoRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, SsaoRenderer& renderer);

// 交换链重建之后调用。离屏附件跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, SsaoRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, SsaoRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SsaoSceneUniform& uniform, FrameStatistics& outStatistics);
