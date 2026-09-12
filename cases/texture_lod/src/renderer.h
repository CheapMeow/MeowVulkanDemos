#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 视图模式，界面、命令行与着色器按同一组取值
enum LodViewMode {
    LOD_VIEW_NORMAL = 0,
    LOD_VIEW_LOD = 1,
    LOD_VIEW_FOOTPRINT = 2,
};

// 采样器随这几项变化，改变时重建采样器并改写描述符
struct LodSamplerOptions {
    bool mipmapped;       // 关闭时把最大层级钳到 0，只采最粗的一级
    float lodBias;
    float maxAnisotropy;
};

// 与着色器中的 SceneBuffer 逐字节对应
struct LodSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 options;  // x 视图模式, y 保留, z 视口宽度, w 保留
};

struct LodFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct LodRenderer {
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount;

    GpuTexture detailTexture;
    VkSampler detailSampler;

    // 深度附件跟随交换链尺寸，本 case 只用它做遮挡，不采样
    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;

    LodSamplerOptions samplerOptions;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    LodFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    uint32_t viewMode;
    LodSamplerOptions sampler;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordDrawMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

void createRenderer(const VulkanContext& ctx, LodRenderer& renderer, const MeshData& groundMesh);
void destroyRenderer(const VulkanContext& ctx, LodRenderer& renderer);

// 交换链重建之后调用。深度附件、呈现帧缓冲与 uniform 里的视口宽度跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, LodRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// FrameInput 里的采样器配置发生变化时，函数在本帧开头重建采样器并改写描述符
bool drawFrame(const VulkanContext& ctx, LodRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const LodSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
