#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

// 反射纹理与金字塔由相邻帧共用，这里只保留一帧在飞
enum { MAX_FRAMES_IN_FLIGHT = 1 };

// 金字塔层级数：第一级是全分辨率深度，之后每一级边长减半
enum { PYRAMID_LEVEL_COUNT = 6 };

// 三种步进方式各对应一条特化过的管线
enum { MARCH_MODE_COUNT = 3 };

// 时间戳查询：几何、金字塔、反射、输出各占一对
enum {
    QUERY_GEOMETRY_BEGIN = 0,
    QUERY_GEOMETRY_END = 1,
    QUERY_PYRAMID_BEGIN = 2,
    QUERY_PYRAMID_END = 3,
    QUERY_REFLECT_BEGIN = 4,
    QUERY_REFLECT_END = 5,
    QUERY_PRESENT_BEGIN = 6,
    QUERY_PRESENT_END = 7,
    QUERY_COUNT = 8
};

// 与着色器中的 SsrUniform 逐字节对应
struct SsrUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::mat4 inverseProjection;
    glm::vec4 viewportParams;   // xy 视口尺寸, z 金字塔层级数, w 保留
    glm::vec4 marchParams;      // x 步数上限, y 步长, z 厚度阈值, w 抖动强度
    glm::vec4 modeParams;       // x 步进模式, y 二分细化, z 累积混合系数, w 抖动开关
    glm::vec4 miscParams;       // x 可视化模式, y 可视化层级, z 屏幕边缘淡出, w 用平均金字塔
    glm::vec4 frameParams;      // x 帧号, y 是否重置累积, z 近平面, w 远平面
    glm::vec4 rangeParams;      // x 反射推进的最大视空间距离, yzw 保留
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
    double cpuRecordGeometryMilliseconds;
    double cpuRecordPyramidMilliseconds;
    double cpuRecordReflectMilliseconds;
    double cpuRecordPresentMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuGeometryMilliseconds;
    double gpuPyramidMilliseconds;
    double gpuReflectMilliseconds;
    double gpuPresentMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
    uint32_t instanceCount;
};

struct SsrFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet frameSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct SsrRenderer {
    GpuBuffer instanceBuffer;
    uint32_t instanceCount;

    GpuTexture colorTexture;     // 场景颜色与粗糙度
    GpuTexture normalTexture;    // 视空间法线与反射强度
    GpuTexture depthTexture;
    GpuTexture pyramidMin;       // 每级取最小值
    GpuTexture pyramidAvg;       // 每级取平均值
    VkImageView pyramidMinMips[PYRAMID_LEVEL_COUNT];
    VkImageView pyramidAvgMips[PYRAMID_LEVEL_COUNT];
    GpuTexture reflectionColor[2];   // rgb 反射颜色乘权重, a 权重
    GpuTexture reflectionStep[2];

    VkFramebuffer sceneFramebuffer;
    VkFramebuffer reflectFramebuffers[2];
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass reflectRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout frameSetLayout;
    VkDescriptorSetLayout pyramidSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline boxPipeline;
    VkPipelineLayout reflectPipelineLayout;
    VkPipeline reflectPipelines[MARCH_MODE_COUNT];
    VkPipelineLayout presentPipelineLayout;
    VkPipeline presentPipeline;
    VkPipelineLayout pyramidPipelineLayout;
    VkPipeline pyramidPipeline;

    VkDescriptorSet pyramidMinSets[PYRAMID_LEVEL_COUNT];
    VkDescriptorSet pyramidAvgSets[PYRAMID_LEVEL_COUNT];

    VkSampler nearestSampler;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    SsrFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

const char* marchModeName(uint32_t mode);
const char* visualizationName(uint32_t visualization);

void createRenderer(const VulkanContext& ctx, SsrRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, SsrRenderer& renderer);

// 交换链重建之后调用。离屏附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, SsrRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, SsrRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SsrUniform& uniform, FrameStatistics& outStatistics);
