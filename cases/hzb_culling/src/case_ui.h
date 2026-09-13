#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    // 是否启用遮挡剔除
    bool occlusionCulling;
    // 金字塔取值取向：0 最远深度, 1 最近深度
    uint32_t pyramidExtreme;
    // 层级选择：0 固定层级, 1 按包围盒屏幕投影尺寸
    uint32_t levelMode;
    // 固定模式下的层级
    uint32_t level;
    // 深度来源：真为当前帧的深度预通道，假为上一帧的深度
    bool currentFrameDepth;
    // 遮挡测试时包围盒的扩大系数
    float expansion;
    // 是否把金字塔的选定层级铺开显示
    bool visualize;
    // 相机的水平角度
    float yawDegrees;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t instanceCount;
    uint32_t visibleInstanceCount;
    uint32_t culledInstanceCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
