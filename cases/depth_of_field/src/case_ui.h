#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    float cameraMoveSpeed;
    // 景深做法，取值见 renderer.h 的 DofMode
    uint32_t mode;
    // 镜头参数：焦距、光圈数、对焦距离
    float focalLength;
    float fNumber;
    float focusDistance;
    // 弥散圆半径上限（像素）
    float maxRadius;
    // 圆盘收集的采样数与抖动强度
    uint32_t sampleCount;
    float jitter;
    // 光圈叶片数，零表示圆形
    uint32_t blades;
    // 显示与场景参数
    float exposure;
    float lightIntensity;
    float ambient;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t renderPassCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
