#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    float cameraMoveSpeed;
    // 色调映射算子，取值见 renderer.h 的 ToneMapOperator
    uint32_t toneMapOperator;
    // 曲线作用在哪些分量上，取值见 renderer.h 的 ToneMapChannelMode
    uint32_t toneMapChannel;
    // 输出编码，取值见 renderer.h 的 ToneMapEncoding
    uint32_t outputEncoding;
    // 曝光，单位 EV，数值上等于 2 的多少次方
    float exposureEv;
    // 扩展 Reinhard 的白点
    float whitePoint;
    // 场景参数：平行光强度、天空亮度、太阳辐射亮度
    float lightIntensity;
    float skyIntensity;
    float sunIntensity;
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
