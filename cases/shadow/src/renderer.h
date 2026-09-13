#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 阴影贴图的分辨率与保存深度值的位数，二者都可以在界面上更改
enum {
    SHADOW_MAP_MIN_SIZE = 256,
    SHADOW_MAP_MAX_SIZE = 4096,
    SHADOW_MAP_DEFAULT_SIZE = 2048,
    SHADOW_MAP_DEFAULT_BITS = 32,
};

// 阴影贴图以颜色附件保存深度值，位数决定贴图的格式：
// 8 位用 R8_UNORM、16 位用 R16_UNORM、32 位用 R32_SFLOAT
VkFormat shadowMapColorFormat(uint32_t bits);

// 阴影模式的取值：关闭、百分比渐近过滤、百分比渐近软阴影
enum ShadowMode {
    SHADOW_MODE_OFF = 0,
    SHADOW_MODE_PCF = 1,
    SHADOW_MODE_PCSS = 2,
};

// 基础深度偏移，单位是光源正交投影下归一化后的深度；界面上可调，调到零就能看到自阴影条纹
constexpr float SHADOW_DEPTH_OFFSET_DEFAULT = 0.0005f;
constexpr float SHADOW_DEPTH_OFFSET_MAX = 0.004f;

// PCF 与 PCSS 的默认参数。PCF 半径为零时退化成一次比较，就是硬阴影
constexpr float SHADOW_PCF_RADIUS_DEFAULT = 1.0f;
constexpr float SHADOW_PCF_RADIUS_MAX = 3.0f;
constexpr float SHADOW_PCSS_SEARCH_RADIUS_DEFAULT = 4.0f;
constexpr float SHADOW_PCSS_SEARCH_RADIUS_MAX = 8.0f;
// 光源半径的单位是世界单位。方向光的光源放在 2.5 倍场景半径处，这个场景的半径在两百以上，
// 半径小的光源角直径微乎其微，半影会小到一个纹素以下，看不到软阴影
constexpr float SHADOW_PCSS_LIGHT_RADIUS_MIN = 25.0f;
constexpr float SHADOW_PCSS_LIGHT_RADIUS_DEFAULT = 400.0f;
constexpr float SHADOW_PCSS_LIGHT_RADIUS_MAX = 2000.0f;
constexpr float SHADOW_PCSS_MIN_PENUMBRA_DEFAULT = 1.0f;
constexpr float SHADOW_PCSS_MAX_PENUMBRA_DEFAULT = 16.0f;
constexpr float SHADOW_PCSS_MAX_PENUMBRA_LIMIT = 64.0f;

// 与着色器中的 SceneBuffer 逐字节对应
struct ShadowSceneUniform {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::mat4 lightViewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 lightDirection;  // xyz 指向光源的单位向量
    glm::vec4 lightColor;      // rgb 颜色, a 强度
    glm::vec4 shadowParams;    // x 基础深度偏移, y 阴影贴图纹素大小, z PCF 半径, w 阴影模式
    glm::vec4 shadowOptions;   // x 法线抬升开关, y 角度偏移开关, z 法线抬升距离, w 近平面的归一化修正量
    glm::vec4 shadowPcss;      // x 遮挡物搜索半径, y 光源半径的纹素尺度, z 最小半影, w 最大半影
};

// 阴影贴图与瑕疵处理的可调配置。尺寸、位数与只写背面深度发生变化时重建阴影资源，
// 着色器侧的法线抬升与角度偏移只改 uniform
struct ShadowOptions {
    uint32_t mapSize;
    uint32_t mapBits;
    bool backFaceDepth;  // 阴影通道只写入背面，用物体厚度换来深度余量
    bool groundCaster;   // 地面也写进阴影贴图，写进去之后地面会与自己比较，出现自阴影条纹
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
    // 物体与地面各自一套顶点与索引缓冲
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

    // 以颜色附件保存深度值的阴影贴图，主通道以采样方式读取，尺寸与位数可以更改
    GpuTexture shadowMap;
    // 阴影通道的深度测试附件，只用来挑出离光源最近的背面，不被采样
    GpuTexture shadowDepth;
    VkSampler shadowSampler;
    VkFramebuffer shadowFramebuffer;
    uint32_t shadowMapSize;
    uint32_t shadowMapBits;
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
    // 地面是单面几何，阴影通道里不做剔除，单独一条管线
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
    uint32_t shadowMode;
    // 阴影配置，与渲染器当前配置不同时在这一帧开始处重建相关资源
    ShadowOptions shadow;
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

void createRenderer(const VulkanContext& ctx, ShadowRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t groundInstanceIndex, const ShadowOptions& shadowOptions);
void destroyRenderer(const VulkanContext& ctx, ShadowRenderer& renderer);

// 交换链重建之后调用。主通道的深度附件与呈现用的帧缓冲跟随交换链尺寸，
// 阴影贴图、渲染通道与管线不受影响
void recreateSwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// FrameInput 里的阴影配置发生变化时，函数在本帧开头重建阴影贴图、渲染通道与管线
bool drawFrame(const VulkanContext& ctx, ShadowRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShadowSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
