#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    float cameraMoveSpeed;
    // 光照模式，取值见 renderer.h 的 BrdfLighting
    uint32_t lighting;
    // 法线分布，取值见 scene_setup.h 的 BrdfDistribution
    uint32_t distribution;
    // 几何项与菲涅耳模型，取值见 renderer.h
    uint32_t geometry;
    uint32_t fresnel;
    // 多次散射补偿开关
    bool multiScattering;
    // 材质参数
    float roughness;
    float metallic;
    float baseColor;
    // 光照参数
    float lightIntensity;
    float environmentRadiance;
    // 炉子测试里片上积分的采样数
    uint32_t furnaceSamples;
    // 显示用的曝光倍数
    float exposure;
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
