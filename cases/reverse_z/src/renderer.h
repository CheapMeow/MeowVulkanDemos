#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 地面分两层，实例缓冲的前两项就是它们
enum { GROUND_LAYER_COUNT = 2 };

// 与着色器中的 SceneBuffer 逐字节对应
struct SceneUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 lightDirection;  // xyz 指向光源的单位向量
    glm::vec4 groundParams;    // x 上下两层地面的高度间距, yzw 保留
};

// 近远裁剪面的取值范围。近裁剪面越小、两者比例越大，标准深度的远处精度越差
constexpr float NEAR_PLANE_MIN = 0.01f;
constexpr float NEAR_PLANE_MAX = 1.0f;
constexpr float FAR_PLANE_MIN = 500.0f;
constexpr float FAR_PLANE_MAX = 200000.0f;
constexpr float GROUND_OFFSET_MIN = 0.001f;
constexpr float GROUND_OFFSET_MAX = 0.2f;

struct ReverseZFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ReverseZRenderer {
    // 两层地面共用一张网格，物体单独一张
    GpuBuffer groundVertexBuffer;
    GpuBuffer groundIndexBuffer;
    uint32_t groundIndexCount;
    GpuBuffer objectVertexBuffer;
    GpuBuffer objectIndexBuffer;
    uint32_t objectIndexCount;

    // 实例缓冲的前两项是两层地面，其后是地面上的物体
    GpuBuffer instanceBuffer;
    uint32_t groundInstanceCount;
    uint32_t objectInstanceOffset;
    uint32_t objectCapacity;

    MaterialTextures material;

    // 主通道的深度附件，跟随交换链尺寸
    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet materialSet;

    VkPipelineLayout pipelineLayout;
    VkPipeline groundPipeline;
    VkPipeline objectPipeline;

    // 当前管线采用的深度模式，与界面上的开关不一致时重建管线
    bool reverseZ;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ReverseZFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    bool reverseZ;
    // 参与绘制的物体数量，两层地面始终绘制
    uint32_t activeObjectCount;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordMainPassMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

void createRenderer(const VulkanContext& ctx, ReverseZRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t objectInstanceOffset, bool reverseZ);
void destroyRenderer(const VulkanContext& ctx, ReverseZRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现用的帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ReverseZRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// FrameInput 里的深度模式与渲染器当前配置不同时，函数在本帧开头重建管线
bool drawFrame(const VulkanContext& ctx, ReverseZRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SceneUniform& sceneUniform, FrameStatistics& outStatistics);
