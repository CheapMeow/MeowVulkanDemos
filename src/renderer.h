#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 绘制路径。三条路径共用同一份着色器与同一套剔除判据，差异只在几何的提交方式
enum DrawPath {
    DRAW_PATH_TRADITIONAL = 0,  // 主机剔除 + 逐可见实例一条 vkCmdDrawIndexed
    DRAW_PATH_INSTANCED = 1,    // 主机剔除 + 一条 vkCmdDrawIndexed，实例数量为可见数量
    DRAW_PATH_INDIRECT = 2      // 计算着色器剔除 + 一条 vkCmdDrawIndexedIndirect
};

enum { DRAW_PATH_COUNT = 3 };

struct GBufferTargets {
    GpuTexture albedoOcclusion;
    GpuTexture normalRoughness;
    GpuTexture positionMetallic;
    GpuTexture depth;
    VkFramebuffer framebuffer;
};

struct MaterialTextures {
    GpuTexture albedo;
    GpuTexture normal;
    GpuTexture metallic;
    GpuTexture roughness;
    GpuTexture ambientOcclusion;
    VkSampler sampler;
};

struct FrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer cameraBuffer;
    GpuBuffer lightBuffer;
    GpuBuffer cpuVisibleBuffer;
    GpuBuffer gpuVisibleBuffer;
    GpuBuffer indirectBuffer;
    GpuBuffer visibleCountReadbackBuffer;

    VkDescriptorSet sceneSet;
    VkDescriptorSet cpuVisibleSet;
    VkDescriptorSet gpuVisibleSet;
    VkDescriptorSet cullSet;
    VkDescriptorSet lightingSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct Renderer {
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount;
    float boundsRadius;

    GpuBuffer instanceBuffer;
    // 缓冲按容量分配，界面上调整的活动数量不会超过它，因此切换数量无需重建资源
    uint32_t instanceCapacity;
    uint32_t lightCapacity;

    MaterialTextures material;
    GBufferTargets gbuffer;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass gbufferRenderPass;
    VkRenderPass lightingRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout visibleSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorSetLayout lightingSetLayout;
    VkDescriptorSetLayout cullSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet materialSet;

    VkPipelineLayout gbufferPipelineLayout;
    VkPipeline gbufferPipeline;
    VkPipelineLayout lightingPipelineLayout;
    VkPipeline lightingPipeline;
    VkPipelineLayout cullPipelineLayout;
    VkPipeline cullPipeline;

    float timestampPeriodNanoseconds;

    FrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的耗时与工作量统计
struct FrameStatistics {
    double cpuCullMilliseconds;
    double cpuRecordMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t visibleInstanceCount;
};

void createRenderer(const VulkanContext& ctx, Renderer& renderer, const MeshData& mesh,
                    const std::vector<InstanceData>& instances, uint32_t lightCapacity);
void destroyRenderer(const VulkanContext& ctx, Renderer& renderer);

// 一帧的绘制输入
struct FrameInput {
    DrawPath drawPath;
    uint32_t activeInstanceCount;
    uint32_t activeLightCount;
    // 界面绘制数据是否记录到本帧，抓取画面时关闭以便三条路径的结果逐像素可比
    bool drawUserInterface;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

void drawFrame(const VulkanContext& ctx, Renderer& renderer, uint64_t frameCounter, const FrameInput& input,
               const CameraUniform& cameraUniform, const std::vector<LightData>& lights,
               const std::vector<InstanceData>& instances, uint32_t* visibleIndices,
               FrameStatistics& outStatistics);
