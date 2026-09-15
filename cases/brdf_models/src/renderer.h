#pragma once

#include "obj_loader.h"
#include "scene_setup.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 光照模式
enum BrdfLighting {
    BRDF_LIGHTING_DIRECTIONAL = 0,  // 一盏方向光
    BRDF_LIGHTING_FURNACE = 1,      // 均匀环境，单次散射在片上积分
    BRDF_LIGHTING_TABLE = 2,        // 均匀环境，直接查方向反照率表作对照
};

// 几何项
enum BrdfGeometry {
    BRDF_GEOMETRY_SMITH = 0,
    BRDF_GEOMETRY_SCHLICK = 1,
    BRDF_GEOMETRY_NONE = 2,
};

// 菲涅耳项
enum BrdfFresnel {
    BRDF_FRESNEL_SCHLICK = 0,
    BRDF_FRESNEL_CONSTANT = 1,
};

// 方向反照率表和它的平均值表在 uniform 里的打包宽度
enum { BRDF_AVERAGE_VEC4_COUNT = (BRDF_AVERAGE_COUNT + 3) / 4 };

// 与着色器中的 BrdfBuffer 逐字节对应
struct BrdfUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    glm::vec4 lightColor;       // rgb 光源颜色, a 均匀环境的辐射亮度
    glm::vec4 material;         // x 粗糙度, y 金属度, z 基础颜色, w 保留
    glm::vec4 options;          // x 光照模式, y 法线分布, z 几何项, w 多次散射补偿开关
    glm::vec4 miscParams;       // x 菲涅耳模型, y 炉子测试采样数, z 曝光倍数, w 保留
    glm::vec4 averageAlbedo[BRDF_AVERAGE_VEC4_COUNT];
};

struct BrdfFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet uniformSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct BrdfRenderer {
    GpuBuffer sphereVertexBuffer;
    GpuBuffer sphereIndexBuffer;
    uint32_t sphereIndexCount;

    // 方向反照率表：x 是法线与视线夹角的余弦，y 是粗糙度
    GpuTexture albedoTable;
    VkSampler tableSampler;
    // 表对应的法线分布，与请求值不同时重建
    uint32_t tableDistribution;
    // 每个粗糙度上的平均方向反照率，建表时一并算出，由调用方填进 uniform
    float averageAlbedo[BRDF_AVERAGE_COUNT];

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

    BrdfFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    // 法线分布变化时重建方向反照率表
    uint32_t distribution;
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

const char* brdfLightingName(uint32_t lighting);
const char* brdfDistributionName(uint32_t distribution);
const char* brdfGeometryName(uint32_t geometry);
const char* brdfFresnelName(uint32_t fresnel);

void createRenderer(const VulkanContext& ctx, BrdfRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, BrdfRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, BrdfRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, BrdfRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const BrdfUniform& uniform, FrameStatistics& outStatistics);
