#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 与着色器中的 CameraBuffer 逐字节对应。前四个字段是与 case 无关的相机矩阵，
// 后面的视锥平面与剔除参数供本 case 的剔除使用
struct CameraUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 frustumPlanes[6];  // xyz 法线, w 常数项, 指向视锥内部为正
    glm::vec4 cullParams;        // x 实例总数, y 模型包围球半径, z 光源数量, w 保留
};

void fillCameraUniform(const Camera& camera, float aspectRatio, uint32_t instanceCount, float boundsRadius,
                       uint32_t lightCount, CameraUniform& outUniform);

// 绘制路径。三条路径共用同一份着色器与同一套剔除判据，差异只在几何的提交方式
enum DrawPath {
    DRAW_PATH_TRADITIONAL = 0,  // 主机剔除 + 逐可见实例一条 vkCmdDrawIndexed
    DRAW_PATH_INSTANCED = 1,    // 主机剔除 + 一条 vkCmdDrawIndexed，实例数量为可见数量
    DRAW_PATH_INDIRECT = 2      // 计算着色器剔除 + 一条 vkCmdDrawIndexedIndirect
};

enum { DRAW_PATH_COUNT = 3 };

// 路径名称，控制台、测量报告与 TCP 分段报告共用
const char* drawPathName(DrawPath drawPath);

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
    // 设备是否支持时间戳查询：驱动报告 timestampComputeAndGraphics 且图形队列族的
    // timestampValidBits 非零才建查询池。桌面独显与多数安卓移动 GPU 都满足
    bool timestampsSupported;

    FrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的耗时与工作量统计，命令缓冲记录过程按功能步骤拆分成独立的耗时项
struct FrameStatistics {
    double cpuCullMilliseconds;
    double cpuRecordBeginMilliseconds;
    double cpuRecordCullDispatchMilliseconds;
    double cpuRecordGBufferPassMilliseconds;
    double cpuRecordLightingPassMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t visibleInstanceCount;
};

void createRenderer(const VulkanContext& ctx, Renderer& renderer, const MeshData& mesh,
                    const std::vector<InstanceData>& instances, uint32_t lightCapacity);
void destroyRenderer(const VulkanContext& ctx, Renderer& renderer);

// 交换链重建之后调用。G-Buffer 的附件、呈现用的帧缓冲与信号量都跟随交换链尺寸与图像数量，
// 需要重新创建；渲染通道与管线只跟格式有关，不受影响
void recreateSwapchainTargets(const VulkanContext& ctx, Renderer& renderer);

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

// 返回 false 表示交换链已经失效（窗口尺寸变化或图像不再适配），这一帧没有绘制任何内容，
// 调用方重建交换链后再画下一帧；outStatistics 在这种情况下没有填完，不要使用
bool drawFrame(const VulkanContext& ctx, Renderer& renderer, uint64_t frameCounter, const FrameInput& input,
               const CameraUniform& cameraUniform, const std::vector<LightData>& lights,
               const std::vector<InstanceData>& instances, uint32_t* visibleIndices,
               FrameStatistics& outStatistics);
