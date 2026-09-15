#pragma once

#include "obj_loader.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 着色频率：光照算在哪一级
enum ShadingFrequency {
    SHADING_FREQUENCY_FLAT = 0,     // 平面着色：每个三角形一个面法线，逐像素着色
    SHADING_FREQUENCY_GOURAUD = 1,  // 顶点着色：每个顶点算一次光照，颜色插值
    SHADING_FREQUENCY_PHONG = 2,    // 逐像素着色：法线插值，每个像素算一次光照
};

// 镜面反射项的模型
enum SpecularModel {
    SPECULAR_MODEL_BLINN_PHONG = 0,  // 半程向量与法线的夹角
    SPECULAR_MODEL_PHONG = 1,        // 反射方向与视线方向的夹角
};

// 逐像素着色时法线的来源
enum NormalSource {
    NORMAL_SOURCE_GEOMETRIC = 0,  // 顶点法线插值
    NORMAL_SOURCE_BUMP = 1,       // 切空间的程序化凹凸
};

// 细分段数的取值范围
enum { SHADING_MIN_SEGMENTS = 6, SHADING_MAX_SEGMENTS = 128 };

// 一帧的绘制配置
struct ShadingOptions {
    uint32_t frequency;
    uint32_t specular;
    uint32_t normalSource;
    uint32_t segments;
    float shininess;
};

// 与着色器中的 ShadingBuffer 逐字节对应
struct ShadingUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    glm::vec4 lightColor;       // rgb 光源颜色, a 环境强度
    glm::vec4 options;          // x 镜面模型, y 法线来源, z 高光指数, w 细分段数
};

struct ShadingFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet uniformSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ShadingRenderer {
    GpuBuffer sphereVertexBuffer;
    GpuBuffer sphereIndexBuffer;
    uint32_t sphereIndexCount;
    // 顶点缓冲对应的细分段数，与请求值不同时重建
    uint32_t sphereSegments;

    GpuTexture depthTexture;

    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout uniformSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline flatPipeline;
    VkPipeline gouraudPipeline;
    VkPipeline phongPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ShadingFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    ShadingOptions options;
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
    uint32_t triangleCount;
};

const char* shadingFrequencyName(uint32_t frequency);
const char* specularModelName(uint32_t specular);
const char* normalSourceName(uint32_t normalSource);

// 球的半径固定为 1，创建时用默认段数生成一次顶点缓冲
void createRenderer(const VulkanContext& ctx, ShadingRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, ShadingRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ShadingRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 细分段数变化时函数在本帧开头重建球的顶点缓冲
bool drawFrame(const VulkanContext& ctx, ShadingRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShadingUniform& uniform, FrameStatistics& outStatistics);
