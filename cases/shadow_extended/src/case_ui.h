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

    // 阴影贴图的分辨率与级联级数
    uint32_t shadowMapSize;
    uint32_t cascadeCount;
    bool cascadeBlend;

    // 瑕疵处理
    bool shadowNormalLift;
    bool shadowSlopeBias;
    float shadowDepthOffset;
    bool shadowGroundCaster;

    // 取值方式、坐标计算位置与可视化
    uint32_t valueMode;
    uint32_t coordMode;
    uint32_t viewMode;

    // PCSS 的遮挡物搜索半径与半影系数
    float blockerRadius;
    float penumbraScale;
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
