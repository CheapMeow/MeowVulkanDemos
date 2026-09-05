#pragma once

#include "gpu_clock_lock.h"
#include "renderer.h"
#include "vk_context.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    DrawPath drawPath;
    int activeInstanceCount;
    int activeLightCount;
    float farPlane;
    float cameraMoveSpeed;
};

// 界面上显示的统计量，均为统计窗口内的平均值
struct UiStatistics {
    double frameMilliseconds;
    double cpuCullMilliseconds;
    double cpuRecordMilliseconds;
    double gpuMilliseconds;
    uint32_t visibleInstanceCount;
    uint32_t drawCallCount;
};

struct UserInterface {
    VkDescriptorPool descriptorPool;
};

void createUserInterface(const VulkanContext& ctx, const Renderer& renderer, UserInterface& ui);
void destroyUserInterface(const VulkanContext& ctx, UserInterface& ui);

// 开始一帧界面，随后构建控件
void beginUserInterfaceFrame();
void buildUserInterface(UiState& state, const UiStatistics& statistics, int maxInstanceCount, int maxLightCount,
                        GpuClockLockState& gpuClockLockState);
// 结束一帧界面，把控件转成绘制数据
void endUserInterfaceFrame();

// 界面是否正在接收键盘输入，此时相机控制让位
bool userInterfaceWantsKeyboard();

// 把界面的绘制数据记录到命令缓冲，必须在渲染通道内调用
void recordUserInterfaceCommands(VkCommandBuffer commandBuffer);
