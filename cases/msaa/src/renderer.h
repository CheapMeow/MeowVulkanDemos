#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 可选的采样数
enum { MSAA_SAMPLE_OPTION_COUNT = 4 };

// 解析方式
enum ResolveMode {
    RESOLVE_HARDWARE = 0,  // 硬件盒式解析
    RESOLVE_CUSTOM = 1,    // 逐采样点编码、求平均、解码
};

// 与着色器中的 SceneBuffer 逐字节对应
struct MsaaSceneUniform {
    glm::vec4 viewportParams;  // xy 视口尺寸
    glm::vec4 colorParams;     // rgb 背景亮度
    glm::vec4 modeParams;      // x 子场景编号, y alpha to coverage, z 片元开销
};

// 一个形状：xy 左下角, z 边长, w 透明度（负值表示镂空形状）
struct ShapeData {
    glm::vec4 shape;
};

// 多重采样相关配置
struct MsaaOptions {
    uint32_t sampleCount;
    uint32_t resolveMode;
    bool alphaToCoverage;
    uint32_t sceneIndex;
    float fragmentCost;
    float backgroundIntensity;
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    MsaaOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordGeometryPassMilliseconds;
    double cpuRecordResolveMilliseconds;
    double cpuRecordTonemapMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t fragmentCount;
};

struct MsaaFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    GpuBuffer counterBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct MsaaRenderer {
    GpuBuffer shapeBuffer;
    uint32_t shapeOffsets[3];
    uint32_t shapeCounts[3];

    // 多重采样附件：颜色用半精度浮点，背景亮度可以超过 1
    GpuTexture msaaColor;
    GpuTexture msaaDepth;
    // 解析目标：单采样浮点贴图，色调映射通道读它
    GpuTexture resolveTexture;

    VkFramebuffer geometryFramebuffer;
    VkFramebuffer resolveFramebuffer;
    std::vector<VkFramebuffer> tonemapFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass geometryRenderPass;
    VkRenderPass resolveRenderPass;
    VkRenderPass tonemapRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout samplerSetLayout;
    VkDescriptorPool descriptorPool;
    // 自定义解析读多重采样的颜色附件，色调映射读解析目标，两者各一个描述符集
    VkDescriptorSet resolveSet;
    VkDescriptorSet samplerSet;

    VkPipelineLayout geometryPipelineLayout;
    VkPipeline geometryOpaquePipeline;
    VkPipeline geometryCoveragePipeline;
    VkPipelineLayout resolvePipelineLayout;
    VkPipeline resolvePipeline;
    VkPipelineLayout tonemapPipelineLayout;
    VkPipeline tonemapPipeline;

    VkSampler sampler;

    // 当前的采样数与解析方式，改动时重建受影响的资源
    uint32_t sampleCount;
    uint32_t resolveMode;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    MsaaFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const uint32_t* msaaSampleOptions();
const char* msaaResolveModeName(uint32_t resolveMode);

void createRenderer(const VulkanContext& ctx, MsaaRenderer& renderer, uint32_t sampleCount,
                    uint32_t resolveMode);
void destroyRenderer(const VulkanContext& ctx, MsaaRenderer& renderer);

// 交换链重建之后调用。多重采样附件与解析目标跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, MsaaRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 采样数或解析方式变化时，函数在本帧开头重建多重采样附件与相关管线
bool drawFrame(const VulkanContext& ctx, MsaaRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const MsaaSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
