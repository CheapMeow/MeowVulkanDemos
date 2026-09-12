#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 阴影贴图的分辨率与可选档位
enum {
    SHADOW_MAP_MIN_SIZE = 256,
    SHADOW_MAP_MAX_SIZE = 4096,
    SHADOW_MAP_DEFAULT_SIZE = 2048,
};

// 级联级数的上限，也是 uniform 里光源矩阵数组的长度
enum { MAX_CASCADE_COUNT = 4 };

// 阴影贴图的取值方式
enum ShadowValueMode {
    SHADOW_VALUE_DEPTH = 0,  // 存深度，比较后取受光比例
    SHADOW_VALUE_VSM = 1,    // 存一阶与二阶矩，用切比雪夫不等式估算
};

// 阴影坐标的计算位置
enum ShadowCoordMode {
    SHADOW_COORD_FRAGMENT = 0,
    SHADOW_COORD_VERTEX = 1,
};

// 界面上的可视化模式
enum ShadowViewMode {
    SHADOW_VIEW_FINAL = 0,
    SHADOW_VIEW_CASCADES = 1,
    SHADOW_VIEW_VISIBILITY = 2,
};

// 与着色器中的 SceneBuffer 逐字节对应
struct ShadowSceneUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::mat4 lightViewProjection[MAX_CASCADE_COUNT];
    glm::vec4 cameraPosition;
    glm::vec4 lightDirection;  // xyz 指向光源的单位向量
    glm::vec4 lightColor;      // rgb 颜色, a 强度
    glm::vec4 shadowParams;    // x 基础深度偏移, y 纹素大小, z PCF 半径, w 阴影开关
    glm::vec4 shadowOptions;   // x 法线抬升开关, y 掠射角放大开关, z 抬升距离, w 保留
    glm::vec4 cascadeSplits;   // 各级的远平面距离，单位是世界空间的视距
    glm::vec4 featureOptions;  // x 级联级数, y 级间融合, z 遮挡物搜索半径, w 半影系数
    glm::vec4 viewOptions;     // x 取值方式, y 坐标计算位置, z 可视化模式, w 摄像机远平面
};

// 阴影相关的可调配置
struct ShadowOptions {
    uint32_t mapSize;
    uint32_t cascadeCount;
    bool cascadeBlend;
    bool normalLift;
    bool slopeBias;
    float depthOffset;
    bool groundCaster;
    uint32_t valueMode;
    uint32_t coordMode;
    float blockerRadius;
    float penumbraScale;
};

struct ShadowFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ShadowRenderer {
    GpuBuffer objectVertexBuffer;
    GpuBuffer objectIndexBuffer;
    uint32_t objectIndexCount;
    GpuBuffer groundVertexBuffer;
    GpuBuffer groundIndexBuffer;
    uint32_t groundIndexCount;

    // 实例缓冲的末尾一项是地面的变换，阴影通道不画地面，主通道用 firstInstance 指向它
    GpuBuffer instanceBuffer;
    uint32_t instanceCapacity;
    uint32_t groundInstanceIndex;

    MaterialTextures material;

    // 每一级一张阴影贴图。级数为 1 时只有第 0 张参与，其余三张仍然绑定在描述符上，
    // 内容不被读取，这样级数变化时描述符可以保持不变
    GpuTexture shadowMaps[MAX_CASCADE_COUNT];
    VkFramebuffer shadowFramebuffers[MAX_CASCADE_COUNT];
    // 每一级共用的深度测试附件，只用来挑出离光源最近的背面
    GpuTexture shadowDepth;
    VkSampler shadowSampler;
    uint32_t shadowMapSize;
    uint32_t cascadeCount;
    uint32_t valueMode;
    bool shadowBackFaceDepth;

    // 主通道的深度附件，跟随交换链尺寸
    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass shadowRenderPass;
    VkRenderPass mainRenderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet materialSet;

    VkPipelineLayout shadowPipelineLayout;
    VkPipeline shadowPipeline;
    VkPipeline shadowGroundPipeline;
    VkPipelineLayout scenePipelineLayout;
    VkPipeline scenePipeline;
    VkPipeline groundPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ShadowFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    uint32_t activeInstanceCount;
    bool drawUserInterface;
    bool shadowsEnabled;
    float pcfRadius;
    ShadowOptions shadow;
    uint32_t viewMode;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordShadowPassMilliseconds;
    double cpuRecordMainPassMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    uint32_t drawCallCount;
};

// 阴影贴图以颜色附件保存深度或矩：深度模式是单通道 R32_SFLOAT，矩模式是两通道 R16G16_SFLOAT
VkFormat shadowMapColorFormat(uint32_t valueMode);
const char* shadowValueModeName(uint32_t valueMode);

void createRenderer(const VulkanContext& ctx, ShadowRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t groundInstanceIndex, const ShadowOptions& shadowOptions);
void destroyRenderer(const VulkanContext& ctx, ShadowRenderer& renderer);

// 交换链重建之后调用。主通道的深度附件与呈现用的帧缓冲跟随交换链尺寸，
// 阴影贴图、渲染通道与管线不受影响
void recreateSwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// 阴影配置里的尺寸、级数与取值方式发生变化时，函数在本帧开头重建阴影资源
bool drawFrame(const VulkanContext& ctx, ShadowRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShadowSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
