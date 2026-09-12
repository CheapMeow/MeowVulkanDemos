#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 绘制顺序
enum DrawOrderMode {
    DRAW_ORDER_FRONT_TO_BACK = 0,
    DRAW_ORDER_BACK_TO_FRONT = 1,
    DRAW_ORDER_RANDOM = 2,
};

// 与着色器中的 InstanceData 逐字节对应
struct QuadInstanceData {
    glm::vec4 positionScale;  // xyz 世界位置, w 半边长
    glm::vec4 color;          // rgb 颜色, a alpha 阈值
    glm::vec4 flags;          // x 是否走 alpha test, yzw 保留
};

// 与着色器中的 SceneBuffer 逐字节对应
struct EarlyZSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 frameParams;  // x 片元开销的循环次数, y 插入永不成立的 discard, z 走 alpha test, w 保留
};

struct EarlyZFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct EarlyZRenderer {
    // 一块面向相机的四边形，所有实例共用
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount;

    // 实例数据不随绘制顺序变化，顺序由单独的索引缓冲给出
    GpuBuffer instanceBuffer;
    GpuBuffer orderBuffer;
    std::vector<QuadInstanceData> instances;
    std::vector<uint32_t> order;
    uint32_t instanceCapacity;

    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline prepassPipeline;
    VkPipeline shadePipeline;

    // 当前绘制顺序与是否使用深度预通道
    uint32_t orderMode;
    bool usePrepass;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    EarlyZFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    uint32_t activeInstanceCount;
    uint32_t orderMode;
    bool usePrepass;
    bool insertDiscard;
    bool alphaTest;
    float fragmentCostIterations;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordPrepassMilliseconds;
    double cpuRecordMainPassMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

// 交叠的四边形实例：沿视线方向一层层排开，位置略有偏移，最后一项是镂空植被。
// 靠前的实例排在列表后面，按列表顺序与逆序就得到后到前与前到后两种绘制顺序
void buildOverlappingQuadInstances(uint32_t quadCount, std::vector<QuadInstanceData>& outInstances);

void createRenderer(const VulkanContext& ctx, EarlyZRenderer& renderer, const MeshData& quadMesh,
                    const std::vector<QuadInstanceData>& instances);
void destroyRenderer(const VulkanContext& ctx, EarlyZRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现用的帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, EarlyZRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 绘制顺序改变时函数在本帧开头重排顺序缓冲，深度预通道开关改变时重建主通道管线
bool drawFrame(const VulkanContext& ctx, EarlyZRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const EarlyZSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
