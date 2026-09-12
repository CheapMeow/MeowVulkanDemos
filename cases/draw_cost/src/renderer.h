#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 提交顺序
enum SubmitOrderMode {
    SUBMIT_ORDER_BY_MATERIAL = 0,  // 按材质分组，每种材质只绑定一次
    SUBMIT_ORDER_ALTERNATING = 1,  // 两种材质交替
    SUBMIT_ORDER_RANDOM = 2,
};

// 与着色器中的 InstanceData 逐字节对应
struct SmallInstanceData {
    glm::vec4 positionScale;  // xyz 世界位置, w 半边长
    glm::vec4 material;       // x 材质编号, yzw 保留
};

// 与着色器中的 SceneBuffer 逐字节对应
struct DrawCostSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 frameParams;  // 保留
};

struct DrawCostFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct DrawCostRenderer {
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount;

    GpuBuffer instanceBuffer;
    GpuBuffer orderBuffer;
    std::vector<SmallInstanceData> instances;
    std::vector<uint32_t> drawList;
    uint32_t instanceCapacity;

    // 材质：每种一个常量缓冲与一个描述符集
    std::vector<GpuBuffer> materialBuffers;
    std::vector<VkDescriptorSet> materialSets;
    uint32_t materialCount;

    GpuTexture depthTexture;
    std::vector<VkFramebuffer> firstFramebuffers;
    std::vector<VkFramebuffer> continueFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    // 第一段渲染通道负责清除，后续段加载已有内容，两者的附件布局衔接一致
    VkRenderPass firstRenderPass;
    VkRenderPass continueRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;

    uint32_t orderMode;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    DrawCostFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    uint32_t activeInstanceCount;
    uint32_t orderMode;
    uint32_t materialCount;
    // 每条命令前重复绑定同一套描述符集与顶点缓冲
    bool redundantBind;
    // 把绘制拆成几段渲染通道
    uint32_t passSplitCount;
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

// 在网格上摆开一批屏幕上只有几个像素的小方块
void buildSmallInstances(uint32_t count, std::vector<SmallInstanceData>& outInstances);

void createRenderer(const VulkanContext& ctx, DrawCostRenderer& renderer, const MeshData& quadMesh,
                    const std::vector<SmallInstanceData>& instances, uint32_t materialCount);
void destroyRenderer(const VulkanContext& ctx, DrawCostRenderer& renderer);

// 交换链重建之后调用。深度附件、两组帧缓冲与呈现用的信号量跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, DrawCostRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, DrawCostRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const DrawCostSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
