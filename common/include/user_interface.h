#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"
#include "vk_context.h"

#include <vulkan/vulkan.h>

// 界面的公共部分：初始化、每帧的开始与结束、字形校验，以及 case 之间共用的面板。
// 每个 case 自己负责窗口、控件与文本
struct UserInterface {
    VkDescriptorPool descriptorPool;
};

// renderPass 是界面最终被记录进去的那个渲染通道。
// caseTexts 必须覆盖 case 自己面板会用到的全部字符串，以及它自己那批计时项的显示名，
// 字形范围与启动时的逐字形校验都以这份集合为准
void createUserInterface(const VulkanContext& ctx, VkRenderPass renderPass,
                         const char* const* caseTexts, int caseTextCount, UserInterface& ui);
void destroyUserInterface(const VulkanContext& ctx, UserInterface& ui);

// 开始一帧界面，随后构建控件
void beginUserInterfaceFrame();
// 结束一帧界面，把控件转成绘制数据
void endUserInterfaceFrame();

// 界面是否正在接收键盘输入，此时相机控制让位
bool userInterfaceWantsKeyboard();

// 把界面的绘制数据记录到命令缓冲，必须在渲染通道内调用
void recordUserInterfaceCommands(VkCommandBuffer commandBuffer);

// GPU 锁频面板，在 case 已经打开的窗口内绘制
void buildGpuClockPanel(GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);

// 耗时面板：逐项列出滑动窗口的均值与标准差，并为每一项画一张最近十秒的曲线。
// 在 case 已经打开的窗口内绘制
void buildTimingPanel(const TimingStore& timing);
