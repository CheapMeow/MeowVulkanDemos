#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    // 是否启用反射
    bool reflection;
    // 步进方式：0 视空间等距, 1 屏幕像素, 2 层次遍历
    uint32_t marchMode;
    // 步数上限
    uint32_t maxSteps;
    // 视空间模式的每步长度
    float stepLength;
    // 屏幕像素模式与层次遍历的每步像素数
    float stepPixels;
    // 命中判定允许的厚度，单位是视空间距离
    float thickness;
    // 反射推进的最大视空间距离
    float maxDistance;
    // 命中之后是否二分细化
    bool binaryRefine;
    // 是否做时域累积
    bool temporal;
    // 是否按粗糙度抖动反射方向
    bool jitter;
    // 抖动强度
    float jitterStrength;
    // 屏幕边缘淡出的宽度
    float edgeFade;
    // 可视化：0 关闭, 1 并排显示两种金字塔, 2 步进次数热力图
    uint32_t visualization;
    // 可视化使用的金字塔层级
    uint32_t visualizeLevel;
    // 层次遍历是否改用取平均的金字塔
    bool averagePyramid;
    // 相机的水平角度
    float yawDegrees;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t instanceCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
