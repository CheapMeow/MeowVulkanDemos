#pragma once

#include "obj_loader.h"
#include "scene_setup.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 背景显示什么
enum ShBackground {
    SH_BACKGROUND_RECONSTRUCTED = 0,  // 用当前阶数的球谐重建环境
    SH_BACKGROUND_ORIGINAL = 1,       // 原始环境
};

// 球的辐照度怎么来
enum ShShading {
    SH_SHADING_COEFFICIENTS = 0,  // 球谐系数加卷积因子
    SH_SHADING_REFERENCE = 1,     // 逐像素在半球上数值积分
};

// 与着色器中的 ShBuffer 逐字节对应
struct ShUniform {
    glm::mat4 viewProjection;
    glm::mat4 inverseViewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 options;          // x 阶数, y 背景模式, z 着色模式, w 参考积分的采样数
    glm::vec4 rotationParams;   // x 环境绕 Y 轴的旋转角, yzw 保留
    glm::vec4 skyParams;        // x 天空亮度, y 太阳辐射亮度, z 太阳余弦内界, w 太阳余弦外界
    glm::vec4 sunDirection;     // xyz 太阳方向, w 保留
    glm::vec4 displayParams;    // x 曝光倍数, yzw 保留
    // 三个通道各七个 vec4，每个通道的前 25 个分量是球谐系数
    glm::vec4 sh[SH_TOTAL_VEC4_COUNT];
};

struct ShFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet uniformSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ShRenderer {
    GpuBuffer sphereVertexBuffer;
    GpuBuffer sphereIndexBuffer;
    uint32_t sphereIndexCount;

    GpuTexture depthTexture;

    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout uniformSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline backgroundPipeline;
    VkPipeline spherePipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ShFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入。模式开关全部通过 uniform 传给着色器，这里只有界面与抓帧
struct FrameInput {
    bool drawUserInterface;
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

const char* shBackgroundName(uint32_t background);
const char* shShadingName(uint32_t shading);

void createRenderer(const VulkanContext& ctx, ShRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, ShRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ShRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, ShRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShUniform& uniform, FrameStatistics& outStatistics);
