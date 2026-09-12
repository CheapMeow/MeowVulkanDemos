#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 插值模式，界面、命令行与着色器按同一组取值
enum InterpolationMode {
    INTERPOLATION_PERSPECTIVE = 0,
    INTERPOLATION_AFFINE = 1,
    INTERPOLATION_DIFFERENCE = 2,
};

enum PatternMode {
    PATTERN_CHECKER = 0,
    PATTERN_GRID = 1,
};

// 与着色器中的 SceneBuffer 逐字节对应
struct InterpolationSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 options;  // x 插值模式, y 图案, z 棋盘格频率, w 保留
};

struct InterpolationFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct InterpolationRenderer {
    // 一块四边形地面，四个顶点，六个索引
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount;

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

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    InterpolationFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    uint32_t interpolationMode;
    uint32_t patternMode;
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

void createRenderer(const VulkanContext& ctx, InterpolationRenderer& renderer, const MeshData& groundMesh);
void destroyRenderer(const VulkanContext& ctx, InterpolationRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现用的帧缓冲跟随交换链尺寸，渲染通道与管线不受影响
void recreateSwapchainTargets(const VulkanContext& ctx, InterpolationRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, InterpolationRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const InterpolationSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
