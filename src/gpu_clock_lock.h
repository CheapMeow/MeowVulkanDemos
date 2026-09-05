#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 最近一次锁频相关操作的结果，界面据此选择要显示的文案
enum class GpuClockLockAction {
    kNone,
    kDetectFailed,
    kDetected,
    kLocked,
    kLockFailed,
    kUnlocked,
    kUnlockFailed,
};

// GPU 锁频面板的完整状态，由界面读取展示、由界面上的操作更新
// 这里的一切都来自 nvidia-smi 的实时查询结果，不缓存任何猜测值
// gpuName 与 lastActionDetail 都来自 nvidia-smi 的英文输出，内容始终是 ASCII，
// 中文文案统一由界面层按 lastAction 组织，这样字体的字形范围校验才能覆盖到全部会显示的文本
struct GpuClockLockState {
    bool detected = false;
    GpuClockLockAction lastAction = GpuClockLockAction::kNone;
    std::string lastActionDetail; // nvidia-smi 相关的诊断信息，没有时为空

    uint32_t gpuIndex = 0;
    std::string gpuName;

    std::vector<uint32_t> supportedCoreClocksMHz;   // 降序排列，下拉框的候选项
    std::vector<uint32_t> supportedMemoryClocksMHz; // 降序排列，下拉框的候选项
    int selectedCoreClockIndex = 0;   // 下拉框当前选中项，索引到 supportedCoreClocksMHz
    int selectedMemoryClockIndex = 0; // 下拉框当前选中项，索引到 supportedMemoryClocksMHz

    bool locked = false;
    uint32_t lockedCoreClockMHz = 0;
    uint32_t lockedMemoryClockMHz = 0;

    uint32_t currentCoreClockMHz = 0;   // 最近一次实时查询到的核心频率
    uint32_t currentMemoryClockMHz = 0; // 最近一次实时查询到的显存频率
    double lastPolledSeconds = -1.0;    // 上一次实时查询的时间戳，用于节流
};

// 程序启动时调用一次：探测第一块 NVIDIA 显卡、读取它支持的核心/显存频率档位
// 探测不到显卡或 nvidia-smi 不可用时把 detected 置为 false 并记录原因，不终止程序
void detectGpuClockLockState(GpuClockLockState& state);

// 每帧调用，内部按 currentSeconds 节流到大约一秒一次，更新当前实时频率
void pollLiveGpuClocks(GpuClockLockState& state, double currentSeconds);

// 按下拉框当前选中的档位调用 nvidia-smi -lgc/-lmc 锁频
void requestLockGpuClocks(GpuClockLockState& state);

// 调用 nvidia-smi -rgc/-rmc 解锁
void requestUnlockGpuClocks(GpuClockLockState& state);

// 程序退出前调用：如果当前处于锁频状态就自动解锁，避免退出后显卡一直卡在固定频率
void releaseGpuClockLockOnExit(GpuClockLockState& state);
