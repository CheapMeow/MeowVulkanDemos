#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 阴影贴图的分辨率，与交换链尺寸无关，只创建一次
enum { SHADOW_MAP_SIZE = 2048 };

// 与着色器中的 SceneBuffer 逐字节对应
struct ShadowSceneUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::mat4 lightViewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 lightDirection;  // xyz 指向光源的单位向量
    glm::vec4 lightColor;      // rgb 颜色, a 强度
    glm::vec4 shadowParams;    // x 深度偏移, y 阴影贴图纹素大小, z PCF 半径, w 阴影开关
};

struct ShadowFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ShadowRenderer {
    // 物体与地面各自一套顶点与索引缓冲
    GpuBuffer objectVertexBuffer;
    GpuBuffer objectIndexBuffer;
    uint32_t objectIndexCount;
    GpuBuffer groundVertexBuffer;
    GpuBuffer groundIndexBuffer;
    uint32_t groundIndexCount;

    // 实例缓冲的末尾一项是地面的变换，阴影通道不画地面，主通道用 firstInstance 指向它
    GpuBuffer instanceBuffer;
    uint32_t instanceCapacity;
    uint32_t groundInstanceIndex;

    MaterialTextures material;

    GpuTexture shadowMap;
    VkSampler shadowSampler;
    VkFramebuffer shadowFramebuffer;
    // 主通道的深度附件，跟随交换链尺寸
    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass shadowRenderPass;
    VkRenderPass mainRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet materialSet;

    VkPipelineLayout shadowPipelineLayout;
    VkPipeline shadowPipeline;
    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipeline groundPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ShadowFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    uint32_t activeInstanceCount;
    bool drawUserInterface;
    bool shadowsEnabled;
    float pcfRadius;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordShadowPassMilliseconds;
    double cpuRecordMainPassMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

void createRenderer(const VulkanContext& ctx, ShadowRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t groundInstanceIndex);
void destroyRenderer(const VulkanContext& ctx, ShadowRenderer& renderer);

// 交换链重建之后调用。主通道的深度附件与呈现用的帧缓冲跟随交换链尺寸，
// 阴影贴图、渲染通道与管线不受影响
void recreateSwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, ShadowRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShadowSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
