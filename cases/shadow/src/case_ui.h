#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    int activeInstanceCount;
    float cameraMoveSpeed;
    float lightYawDegrees;
    float lightPitchDegrees;
    bool shadowsEnabled;
    bool pcfEnabled;
    // 阴影贴图的分辨率与保存深度值的位数，改动后渲染器会重建阴影资源
    uint32_t shadowMapSize;
    uint32_t shadowMapBits;
    // 瑕疵处理的三项措施与基础深度偏移，关闭或调零都能观察对应的阴影瑕疵
    bool shadowBackFaceDepth;
    bool shadowNormalLift;
    bool shadowSlopeBias;
    float shadowDepthOffset;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor);
