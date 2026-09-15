#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    float cameraMoveSpeed;
    // 球谐阶数，1 到 5
    uint32_t bandCount;
    // 背景显示什么，取值见 renderer.h 的 ShBackground
    uint32_t background;
    // 球的辐照度怎么来，取值见 renderer.h 的 ShShading
    uint32_t shading;
    // 逐像素参考积分的采样数
    uint32_t referenceSamples;
    // 环境绕 Y 轴的旋转角度，单位度
    float rotationDegrees;
    // 显示用的曝光倍数
    float exposure;
    // 环境参数：天空亮度、光晕亮度与光晕锐度
    float skyIntensity;
    float glowIntensity;
    float glowExponent;
    // 上一次投影的用时，单位毫秒，只用于显示
    double projectionMilliseconds;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
