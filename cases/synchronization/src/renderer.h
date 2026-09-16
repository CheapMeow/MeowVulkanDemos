#pragma once

#include "vk_resources.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

enum { MAX_FRAMES_IN_FLIGHT = 2 };

// 同一个依赖（计算着色器写出的图案被光栅化消费者读取）用不同的同步方式表达。
// 每种方式都要给出同样的画面，差别在命令、掩码、主机是否阻塞与耗时
enum SyncMode {
    SYNC_MODE_BARRIER = 0,         // 管线屏障：一次提交，命令缓冲内 vkCmdPipelineBarrier
    SYNC_MODE_EVENT,               // 事件：一次提交，vkCmdSetEvent 与 vkCmdWaitEvents 分居两端
    SYNC_MODE_SEMAPHORE_BINARY,    // 二进制信号量：两次提交，队列内两批之间
    SYNC_MODE_SEMAPHORE_TIMELINE,  // 时间线信号量：两次提交，主机还能查询计数值
    SYNC_MODE_FENCE,               // 围栏：两次提交，主机在中间等待
    SYNC_MODE_QUEUE_IDLE,          // 队列空闲等待：两次提交，主机等待整条队列
    SYNC_MODE_DEVICE_IDLE,         // 设备空闲等待：两次提交，主机等待整个设备
    SYNC_MODE_SUBPASS,             // 子通道依赖：生产与消费在同一个渲染通道的两个子通道里
    SYNC_MODE_COUNT
};

// 与着色器中的 PatternUniform 逐字节对应
struct PatternUniform {
    glm::vec4 resolutionAndPhase;  // xy 图案尺寸, z 相位, w 叠加层数
    glm::vec4 extra;               // 保留
};

struct SyncFrameResources {
    VkCommandBuffer producerCommandBuffer;
    VkCommandBuffer consumerCommandBuffer;

    VkSemaphore imageAvailable;      // 交换链图像就绪
    VkSemaphore patternReadyBinary;  // 二进制信号量：生产者完成
    VkFence inFlight;                // 本组帧资源已经可以复用
    VkFence patternSync;             // 围栏：生产者完成
    VkEvent patternEvent;            // 事件：生产者在设备上置位

    GpuBuffer uniformBuffer;
    bool uniformBufferCoherent;         // 参数缓冲所在内存类型是否与主机一致
    bool uniformFlushPerformed;         // 本机是否真的走了显式冲洗这条路

    VkDescriptorSet computeSet;         // 图案作为存储图像
    VkDescriptorSet rasterProducerSet;  // 只用到参数块
    VkDescriptorSet sampledConsumerSet; // 图案作为组合图像采样器
    VkDescriptorSet inputConsumerSet;   // 图案作为输入附件

    VkQueryPool timestampPool;
    bool timestampsValid;
};

struct SynchronizationRenderer {
    GpuTexture patternTexture;
    VkSampler patternSampler;

    // 时间线信号量全部帧共用，计数值逐帧递增
    VkSemaphore patternReadyTimeline;
    // 查询计数值的入口点。安卓的运行库桩里没有这个 1.2 核心入口点，按扩展的规定
    // 从 vkGetDeviceProcAddr 取
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue;

    VkRenderPass consumerRenderPass;  // 单个子通道：消费图案并写交换链图像
    VkRenderPass subpassRenderPass;   // 两个子通道：先写图案再读图案
    std::vector<VkFramebuffer> consumerFramebuffers;
    std::vector<VkFramebuffer> subpassFramebuffers;
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<VkFence> imageFences;

    VkDescriptorSetLayout computeSetLayout;
    VkDescriptorSetLayout rasterProducerSetLayout;
    VkDescriptorSetLayout sampledConsumerSetLayout;
    VkDescriptorSetLayout inputConsumerSetLayout;
    VkDescriptorPool descriptorPool;

    VkPipelineLayout computePipelineLayout;
    VkPipelineLayout rasterProducerPipelineLayout;
    VkPipelineLayout sampledConsumerPipelineLayout;
    VkPipelineLayout inputConsumerPipelineLayout;

    VkPipeline computePipeline;
    VkPipeline rasterProducerPipeline;
    VkPipeline sampledConsumerPipeline;
    VkPipeline inputConsumerPipeline;

    float timestampPeriodNanoseconds;
    bool timestampsSupported;

    SyncFrameResources frames[MAX_FRAMES_IN_FLIGHT];
};

// 一帧的绘制输入。图案参数走 PatternUniform，这里只放与同步方式有关的项
struct FrameInput {
    bool drawUserInterface;
    uint32_t syncMode;
    // 非空时把本帧结果拷回该缓冲
    const GpuBuffer* captureBuffer;
};

// 一帧的各阶段耗时与工作量
struct FrameStatistics {
    double cpuRecordProducerMilliseconds;
    double cpuRecordSyncMilliseconds;
    double cpuRecordConsumerMilliseconds;
    double cpuRecordUiMilliseconds;
    double cpuRecordCaptureMilliseconds;
    double cpuRecordSubmitMilliseconds;
    double hostWaitMilliseconds;
    double gpuProducerMilliseconds;
    double gpuConsumerMilliseconds;
    uint32_t drawCallCount;
    uint32_t commandBufferCount;
    uint64_t timelineValue;
};

// 报告、控制台与控制命令里使用的英文名字
const char* syncModeName(uint32_t mode);
// 从名字解析同步方式，返回 false 表示名字无效
bool parseSyncMode(const char* name, uint32_t& outMode);

// 当前同步方式使用的阶段掩码与访问掩码，供界面显示。两行文本与 renderer.cpp 里实际
// 记录的命令一一对应
void syncModeMaskSummary(uint32_t mode, const char*& outFirstLine, const char*& outSecondLine);

// 界面上显示图案与消费结果时需要知道当前处于哪种同步方式下，据此选择渲染通道
uint32_t syncModeRenderPassFamily(uint32_t mode);

void createRenderer(const VulkanContext& ctx, SynchronizationRenderer& renderer);
void destroyRenderer(const VulkanContext& ctx, SynchronizationRenderer& renderer);

// 交换链重建之后调用。图案纹理、帧缓冲与呈现用信号量跟随交换链尺寸与图像数量
void recreateSwapchainTargets(const VulkanContext& ctx, SynchronizationRenderer& renderer);

// 返回 false 表示交换链已经失效，这一帧没有绘制任何内容，调用方重建交换链后再画下一帧
bool drawFrame(const VulkanContext& ctx, SynchronizationRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const PatternUniform& uniform, FrameStatistics& outStatistics);
