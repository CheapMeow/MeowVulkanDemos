#pragma once

#include "scene_setup.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 重建核
enum FilterMode {
    FILTER_NEAREST = 0,
    FILTER_BILINEAR = 1,
    FILTER_BSPLINE = 2,
    FILTER_CATMULL_ROM = 3,
    FILTER_LANCZOS2 = 4,
    FILTER_LANCZOS3 = 5,
    FILTER_ANALYTIC = 6,
};

// 与着色器中的 SampleBuffer 逐字节对应
struct SampleUniform {
    glm::vec4 params;   // x 过滤方式, y 缩放, z 纹理尺寸, w 参考的超采样数（每边的平方根）
    glm::vec4 misc;     // x 显示缩放, yzw 保留
};

struct FrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet sampleSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct FilterRenderer {
    GpuTexture patternTexture;
    VkSampler patternSampler;

    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout sampleSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline samplePipeline;

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
    double cpuRecordSampleMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

const char* filterModeName(uint32_t mode);
// 每种核一次采样的纹理读取次数，用于在界面上显示工作量
uint32_t filterTapCount(uint32_t mode, uint32_t referenceSide);

void createRenderer(const VulkanContext& ctx, FilterRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, FilterRenderer& renderer);

// 交换链重建之后调用。呈现帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, FilterRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, FilterRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SampleUniform& uniform, FrameStatistics& outStatistics);
