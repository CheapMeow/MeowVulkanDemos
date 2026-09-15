#pragma once

#include "obj_loader.h"
#include "vk_resources.h"

#include <glm/glm.hpp>

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 色调映射算子
enum ToneMapOperator {
    TONE_MAP_OP_NONE = 0,               // 不做映射，直接截断到 [0,1]
    TONE_MAP_OP_REINHARD = 1,           // Reinhard
    TONE_MAP_OP_REINHARD_EXTENDED = 2,  // 带白点的扩展 Reinhard
    TONE_MAP_OP_ACES = 3,               // ACES 的解析拟合
    TONE_MAP_OP_FILMIC = 4,             // Hable 的 filmic 曲线
};

// 曲线作用在哪些分量上
enum ToneMapChannelMode {
    TONE_MAP_CHANNEL_PER_CHANNEL = 0,  // 三个分量各自过曲线
    TONE_MAP_CHANNEL_LUMINANCE = 1,    // 只压缩亮度，颜色比例保留
};

// 输出编码
enum ToneMapEncoding {
    TONE_MAP_ENCODING_LINEAR = 0,  // 线性直写
    TONE_MAP_ENCODING_GAMMA = 1,   // 伽马 2.2
    TONE_MAP_ENCODING_SRGB = 2,    // sRGB 传递函数
};

// 一帧的色调映射配置
struct ToneMapOptions {
    uint32_t op;
    uint32_t channel;
    uint32_t encoding;
    float exposureEv;
    float whitePoint;
};

// 与着色器中的 ToneMapBuffer 逐字节对应
struct ToneMapUniform {
    glm::mat4 viewProjection;
    glm::mat4 inverseViewProjection;
    glm::vec4 cameraPosition;   // xyz 相机位置, w 保留
    glm::vec4 lightDirection;   // xyz 指向光源的单位向量, w 光照强度
    glm::vec4 sunDirection;     // xyz 指向太阳的单位向量, w 太阳辐射亮度
    glm::vec4 skyParams;        // x 天空整体亮度, y 天顶到地平线的衰减指数, zw 保留
    glm::vec4 operatorParams;   // x 算子, y 作用方式, z 曝光倍数, w 白点
    glm::vec4 encodingParams;   // x 输出编码, yzw 保留
};

struct ToneMapFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer uniformBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct ToneMapRenderer {
    GpuBuffer objectVertexBuffer;
    GpuBuffer objectIndexBuffer;
    uint32_t objectIndexCount;

    GpuBuffer barVertexBuffer;
    GpuBuffer barIndexBuffer;
    uint32_t barIndexCount;

    GpuTexture albedo;
    GpuTexture normal;
    VkSampler sampler;

    // 场景通道写出的高动态范围图像，色调映射通道读取它
    GpuTexture hdrScene;
    GpuTexture depthTexture;

    VkFramebuffer sceneFramebuffer;
    std::vector<VkFramebuffer> outputFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass sceneRenderPass;
    VkRenderPass outputRenderPass;

    VkDescriptorSetLayout uniformSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout scenePipelineLayout;
    VkPipeline skyPipeline;
    VkPipeline objectPipeline;
    VkPipeline barPipeline;
    VkPipeline tonemapPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    ToneMapFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    ToneMapOptions options;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordBeginMilliseconds;
    double cpuRecordSceneMilliseconds;
    double cpuRecordTonemapMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double gpuMilliseconds;
    double gpuSceneMilliseconds;
    double gpuTonemapMilliseconds;
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

const char* toneMapOperatorName(uint32_t op);
const char* toneMapChannelName(uint32_t channel);
const char* toneMapEncodingName(uint32_t encoding);

void createRenderer(const VulkanContext& ctx, ToneMapRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& barMesh);
void destroyRenderer(const VulkanContext& ctx, ToneMapRenderer& renderer);

// 交换链重建之后调用。高动态范围附件、深度附件与输出帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, ToneMapRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, ToneMapRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ToneMapUniform& uniform, FrameStatistics& outStatistics);
