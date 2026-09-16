#pragma once

#include "scene_setup.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 后处理里的景深做法
enum DofMode {
    DOF_MODE_OFF = 0,       // 不做处理，相当于一个针孔相机
    DOF_MODE_GAUSSIAN = 1,  // 按中心像素的弥散圆铺一圈固定权重
    DOF_MODE_DISK = 2,      // 弥散圆感知的圆盘收集
};

// 与着色器中的 SceneBuffer 逐字节对应
struct SceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    glm::vec4 sceneParams;      // x 环境项, yzw 保留
};

// 与着色器中的 DofBuffer 逐字节对应
struct DofUniform {
    glm::vec4 cameraParams;   // x 近平面, y 远平面, z 画面宽度, w 画面高度
    glm::vec4 lensParams;     // x 焦距, y 光圈数, z 对焦距离, w 弥散圆半径上限（像素）
    glm::vec4 modeParams;     // x 处理方式, y 采样数, z 抖动强度, w 光圈叶片数（零表示圆形）
    glm::vec4 miscParams;     // x 曝光倍数, yzw 保留
};

struct FrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    GpuBuffer dofBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet dofSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct DofRenderer {
    GpuBuffer sceneVertexBuffer;
    GpuBuffer sceneIndexBuffer;
    uint32_t sceneIndexCount;

    GpuTexture hdrScene;
    // 深度附件同时当纹理采样，景深要用它还原视空间距离
    GpuTexture depthTexture;
    VkSampler depthSampler;
    VkSampler colorSampler;

    VkFramebuffer sceneFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout dofSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipelineLayout dofPipelineLayout;
    VkPipeline dofPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    FrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordPostMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    double gpuSceneMilliseconds;
    double gpuPostMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

const char* dofModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, DofRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, DofRenderer& renderer);

// 交换链重建之后调用。高动态范围附件、深度附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, DofRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, DofRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SceneUniform& sceneUniform,
               const DofUniform& dofUniform, FrameStatistics& outStatistics);
