#include "timing.h"

#include <algorithm>
#include <cmath>

const char* timingDisplayName(TimingId id)
{
    static const char* const names[TIMING_ID_COUNT] = {
        "帧时间",
        "主机剔除",
        "记录：命令缓冲起始",
        "记录：剔除计算调度",
        "记录：G-Buffer 通道",
        "记录：光照通道",
        "记录：界面绘制",
        "记录：抓帧拷贝",
        "记录：提交命令",
        "设备时间",
    };
    return names[id];
}

const char* timingReportColumnName(TimingId id)
{
    static const char* const names[TIMING_ID_COUNT] = {
        "frame_ms",
        "cpu_cull_ms",
        "cpu_record_begin_ms",
        "cpu_record_cull_dispatch_ms",
        "cpu_record_gbuffer_pass_ms",
        "cpu_record_lighting_pass_ms",
        "cpu_record_ui_ms",
        "cpu_record_capture_ms",
        "cpu_record_submit_ms",
        "gpu_ms",
    };
    return names[id];
}

void initTimingStore(TimingStore& store, double startTime)
{
    store = TimingStore();
    store.startTime = startTime;
}

void resetTimingWindows(TimingStore& store)
{
    for (int i = 0; i < TIMING_ID_COUNT; ++i) {
        store.window[i] = TimingWindow();
    }
    store.windowCursor = 0;
}

// 用当前窗口内的样本重新计算均值与标准差，窗口容量固定为 100，开销可以忽略
static void updateWindowStatistics(TimingWindow& window)
{
    if (window.sampleCount == 0) {
        window.mean = 0.0;
        window.standardDeviation = 0.0;
        window.minValue = 0.0;
        window.maxValue = 0.0;
        return;
    }

    double sum = 0.0;
    double minValue = window.samples[0];
    double maxValue = window.samples[0];
    for (int i = 0; i < window.sampleCount; ++i) {
        sum += window.samples[i];
        minValue = std::min(minValue, window.samples[i]);
        maxValue = std::max(maxValue, window.samples[i]);
    }
    const double mean = sum / static_cast<double>(window.sampleCount);

    double sumSquaredDelta = 0.0;
    for (int i = 0; i < window.sampleCount; ++i) {
        const double delta = window.samples[i] - mean;
        sumSquaredDelta += delta * delta;
    }
    const double variance =
        window.sampleCount > 1 ? sumSquaredDelta / static_cast<double>(window.sampleCount - 1) : 0.0;

    window.mean = mean;
    window.standardDeviation = std::sqrt(variance);
    window.minValue = minValue;
    window.maxValue = maxValue;
}

static void pushReportSample(TimingReportAccumulator& accumulator, double value)
{
    ++accumulator.sampleCount;
    const double delta = value - accumulator.mean;
    accumulator.mean += delta / static_cast<double>(accumulator.sampleCount);
    const double delta2 = value - accumulator.mean;
    accumulator.sumSquaredDelta += delta * delta2;
}

double timingReportStandardDeviation(const TimingReportAccumulator& accumulator)
{
    if (accumulator.sampleCount < 2) {
        return 0.0;
    }
    const double variance = accumulator.sumSquaredDelta / static_cast<double>(accumulator.sampleCount - 1);
    return std::sqrt(variance);
}

void recordFrameTimingSamples(TimingStore& store, double currentTime, bool includeInReport,
                              const double values[TIMING_ID_COUNT])
{
    const float elapsedSeconds = static_cast<float>(currentTime - store.startTime);
    store.historyTimeSeconds.push_back(elapsedSeconds);

    for (int i = 0; i < TIMING_ID_COUNT; ++i) {
        const double value = values[i];

        store.historyValues[i].push_back(static_cast<float>(value));

        TimingWindow& window = store.window[i];
        window.samples[store.windowCursor] = value;
        if (window.sampleCount < TIMING_WINDOW_CAPACITY) {
            ++window.sampleCount;
        }
        updateWindowStatistics(window);

        if (includeInReport) {
            pushReportSample(store.report[i], value);
        }
    }

    store.windowCursor = (store.windowCursor + 1) % TIMING_WINDOW_CAPACITY;
}
