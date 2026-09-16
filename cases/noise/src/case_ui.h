#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    // 噪声种类，取值见 renderer.h 的 NoiseKind
    uint32_t kind;
    // 基础频率：每单位纹理坐标里放多少个格子
    float frequency;
    // 多倍频叠加的参数
    uint32_t octaves;
    float persistence;
    float lacunarity;
    // 参考路径的超采样数，每边开平方个
    uint32_t referenceSide;
    // 图案平移用的时间，单位秒
    float timeSeconds;
    bool animate;
    // 显示缩放
    float displayScale;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t renderPassCount;
    // 本帧每个像素的噪声求值次数
    uint32_t evaluationCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
