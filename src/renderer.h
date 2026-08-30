#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 绘制路径
enum DrawPath {
    DRAW_PATH_TRADITIONAL = 0,  // CPU 剔除 + 逐实例 vkCmdDrawIndexed
    DRAW_PATH_INDIRECT = 1      // 计算着色器剔除 + 一次 vkCmdDrawIndexedIndirect
};

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
    uint32_t instanceCount;
    uint32_t lightCount;

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
                    const std::vector<InstanceData>& instances, uint32_t lightCount);
void destroyRenderer(const VulkanContext& ctx, Renderer& renderer);

// 提交一帧。captureBuffer 非空时把本帧结果拷回该缓冲
void drawFrame(const VulkanContext& ctx, Renderer& renderer, uint64_t frameCounter, DrawPath drawPath,
               const CameraUniform& cameraUniform, const std::vector<LightData>& lights,
               const std::vector<InstanceData>& instances, uint32_t* visibleIndices, float boundsRadius,
               const GpuBuffer* captureBuffer, FrameStatistics& outStatistics);
