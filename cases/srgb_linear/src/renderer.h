#pragma once

#include "obj_loader.h"
#include "scene.h"
#include "vk_resources.h"

#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 输出的色调映射方式
enum ToneMapMode {
    TONE_MAP_NONE = 0,
    TONE_MAP_REINHARD = 1,
    TONE_MAP_ACES = 2,
};

// 颜色空间相关的可调配置。两张贴图的图像是同一份数据，切换的是绑上来的视图
struct SrgbOptions {
    bool albedoSrgb;
    bool normalSrgb;
    bool gammaOutput;
    uint32_t toneMapMode;
};

// 与着色器中的 SceneBuffer 逐字节对应
struct SrgbSceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 lightDirection;  // xyz 指向光源的单位向量
    glm::vec4 options;         // x 反照率按 sRGB 解释, y 法线按 sRGB 解释, z 输出编码, w 色调映射
};

struct SrgbFrameResources {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;

    GpuBuffer sceneBuffer;
    VkDescriptorSet sceneSet;

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct SrgbRenderer {
    GpuBuffer objectVertexBuffer;
    GpuBuffer objectIndexBuffer;
    uint32_t objectIndexCount;

    GpuBuffer barVertexBuffer;
    GpuBuffer barIndexBuffer;
    uint32_t barIndexCount;

    // 两张贴图各有线性与 sRGB 两个视图，指向同一份数据
    GpuTexture albedo;
    VkImageView albedoAlternateView;
    GpuTexture normal;
    VkImageView normalAlternateView;
    VkSampler sampler;

    // 深度附件跟随交换链尺寸，本 case 只用它做遮挡，不采样
    GpuTexture depthTexture;

    std::vector<VkFramebuffer> presentFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkRenderPass renderPass;

    VkDescriptorSetLayout sceneSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout pipelineLayout;
    VkPipeline objectPipeline;
    VkPipeline barPipeline;

    SrgbOptions currentOptions;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    SrgbFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入
struct FrameInput {
    bool drawUserInterface;
    SrgbOptions options;
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

void createRenderer(const VulkanContext& ctx, SrgbRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& barMesh);
void destroyRenderer(const VulkanContext& ctx, SrgbRenderer& renderer);

// 交换链重建之后调用。深度附件与呈现帧缓冲跟随交换链尺寸
void recreateSwapchainTargets(const VulkanContext& ctx, SrgbRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧。
// FrameInput 里的配置发生变化时，函数在本帧开头改写描述符里的贴图视图
bool drawFrame(const VulkanContext& ctx, SrgbRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SrgbSceneUniform& sceneUniform,
               FrameStatistics& outStatistics);
