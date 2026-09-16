#pragma once

#include "scene_setup.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 后处理里的调色方式
enum GradeMode {
    GRADE_MODE_NONE = 0,      // 不做处理
    GRADE_MODE_LUT3D = 1,     // 三维查找表
    GRADE_MODE_CURVES = 2,    // 每通道一维曲线
    GRADE_MODE_ANALYTIC = 3,  // 解析变换，作为对照的真值
};

// 与着色器中的 SceneBuffer 逐字节对应
struct SceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    glm::vec4 sceneParams;      // x 环境项, yzw 保留
};

// 与着色器中的 GradeBuffer 逐字节对应
struct GradeUniform {
    glm::vec4 params;   // x 处理方式, y 三维表格尺寸, z 曝光倍数, w 保留
};

struct FrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    GpuBuffer gradeBuffer;
    VkDescriptorSet sceneSet;
    VkDescriptorSet gradeSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct GradeRenderer {
    GpuBuffer sphereVertexBuffer;
    GpuBuffer sphereIndexBuffer;
    uint32_t sphereIndexCount;

    GpuBuffer patchVertexBuffer;
    GpuBuffer patchIndexBuffer;
    uint32_t patchIndexCount;

    // 三维查找表与一维曲线，表格尺寸变化时重建
    GpuTexture lutTexture;
    GpuTexture curveTexture;
    uint32_t lutSize;
    VkSampler lutSampler;

    GpuTexture hdrScene;
    GpuTexture depthTexture;

    VkFramebuffer sceneFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout gradeSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline backgroundPipeline;
    VkPipeline spherePipeline;
    VkPipeline patchPipeline;
    VkPipelineLayout gradePipelineLayout;
    VkPipeline gradePipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    FrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    uint32_t gradeMode;
    uint32_t lutSize;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordGradeMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    double gpuSceneMilliseconds;
    double gpuGradeMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

const char* gradeModeName(uint32_t mode);

void createRenderer(const VulkanContext& ctx, GradeRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, GradeRenderer& renderer);

// 交换链重建之后调用。高动态范围附件、深度附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, GradeRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 表格尺寸变化时函数在本帧开头重建两张查找表
bool drawFrame(const VulkanContext& ctx, GradeRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SceneUniform& sceneUniform,
               const GradeUniform& gradeUniform, FrameStatistics& outStatistics);
