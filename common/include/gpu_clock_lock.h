#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 最近一次锁频相关操作的结果，界面据此选择要显示的文案
enum class GpuClockLockAction {
    kNone,
    kDetectFailed,
    kDetected,
    kQueryFailed,
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

    bool clocksQueried = false;         // 是否已经成功查询过一次实时频率
    uint32_t currentCoreClockMHz = 0;   // 最近一次实时查询到的核心频率
    uint32_t currentMemoryClockMHz = 0; // 最近一次实时查询到的显存频率
};

// 程序启动时调用一次：探测第一块 NVIDIA 显卡、读取它支持的核心/显存频率档位
// 探测不到显卡或 nvidia-smi 不可用时把 detected 置为 false 并记录原因，不终止程序
void detectGpuClockLockState(GpuClockLockState& state);

// 只在界面上按下查询按钮时调用一次，更新当前实时频率。启动 nvidia-smi 会阻塞主线程
// 几十到一百多毫秒，因此不做任何定时轮询，免得帧时间曲线被这种与绘制无关的因素干扰
void queryLiveGpuClocks(GpuClockLockState& state);

// 后台线程按固定间隔采样实时频率，界面上的频率曲线读这些样本
struct GpuClockMonitor {
    bool detected = false;
    uint32_t gpuIndex = 0;

    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable wakeUp;
    bool stopRequested = false;

    std::vector<float> timeSeconds;    // 采样时刻，程序启动以来经过的秒数
    std::vector<float> coreClockMHz;
    std::vector<float> memoryClockMHz;
};

// 采样间隔，单位秒
enum { GPU_CLOCK_SAMPLE_PERIOD_SECONDS = 1 };

// 启动后台采样线程。startTime 是程序启动时刻，采样时间戳以它为零点，与耗时曲线共用同一条横轴。
// 探测不到显卡时不启动，界面上也就没有频率曲线
void startGpuClockMonitor(GpuClockMonitor& monitor, const GpuClockLockState& state, double startTime);

// 通知采样线程结束并等待它退出
void stopGpuClockMonitor(GpuClockMonitor& monitor);

// 界面每帧调用，在锁内拷出采样历史
void copyGpuClockSamples(const GpuClockMonitor& monitor, std::vector<float>& outTimeSeconds,
                         std::vector<float>& outCoreClockMHz, std::vector<float>& outMemoryClockMHz);

// 按下拉框当前选中的档位调用 nvidia-smi -lgc/-lmc 锁频
void requestLockGpuClocks(GpuClockLockState& state);

// 按命令行传入的目标频率锁频：在两个档位列表里各取最接近请求值的一项，选中它再锁定。
// 探测不到显卡或者档位列表为空时什么都不做，返回 false 并说明原因；驱动拒绝锁频时
// 同样返回 false，并把 nvidia-smi 的输出打出来
bool requestGpuClockLockFromCommandLine(GpuClockLockState& state, uint32_t requestedCoreMHz,
                                        uint32_t requestedMemoryMHz);

// 调用 nvidia-smi -rgc/-rmc 解锁
void requestUnlockGpuClocks(GpuClockLockState& state);

// 程序退出前调用：如果当前处于锁频状态就自动解锁，避免退出后显卡一直卡在固定频率
void releaseGpuClockLockOnExit(GpuClockLockState& state);
