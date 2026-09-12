#include "timing.h"

#include <chrono>
#include <cmath>
#include <cstdio>

double nowSeconds()
{
    static const std::chrono::steady_clock::time_point firstCall = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - firstCall;
    return elapsed.count();
}

void initTimingStore(TimingStore& store, const TimingItemDescription* items, int itemCount, double startTime)
{
    store = TimingStore();
    store.items = items;
    store.itemCount = itemCount;
    store.startTime = startTime;
}

void resetTimingWindows(TimingStore& store)
{
    for (int i = 0; i < store.itemCount; ++i) {
        store.window[i] = TimingWindow();
    }
    store.windowCursor = 0;
}

void resetTimingReport(TimingStore& store)
{
    for (int i = 0; i < store.itemCount; ++i) {
        store.report[i] = TimingReportAccumulator();
    }
}

// 用当前窗口内的样本重新计算均值与标准差，窗口容量固定为 100，开销可以忽略
static void updateWindowStatistics(TimingWindow& window)
{
    if (window.sampleCount == 0) {
        window.mean = 0.0;
        window.standardDeviation = 0.0;
        return;
    }

    double sum = 0.0;
    for (int i = 0; i < window.sampleCount; ++i) {
        sum += window.samples[i];
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

std::string timingReportHeaderColumns(const TimingStore& store)
{
    std::string columns;
    for (int i = 0; i < store.itemCount; ++i) {
        columns.append(",");
        columns.append(store.items[i].reportColumn);
        columns.append("_avg,");
        columns.append(store.items[i].reportColumn);
        columns.append("_stddev");
    }
    return columns;
}

std::string timingReportValueColumns(const TimingStore& store)
{
    std::string columns;
    for (int i = 0; i < store.itemCount; ++i) {
        const TimingReportAccumulator& accumulator = store.report[i];
        char numbers[48];
        const int numberLength = std::snprintf(numbers, sizeof(numbers), ",%.3f,%.3f", accumulator.mean,
                                               timingReportStandardDeviation(accumulator));
        columns.append(numbers, static_cast<size_t>(numberLength));
    }
    return columns;
}

void appendMeasurementReport(const std::string& path, const std::string& headerColumns,
                             const std::string& valueColumns)
{
    std::FILE* probeFile = std::fopen(path.c_str(), "rb");
    bool needsHeader = true;
    if (probeFile != nullptr) {
        std::fseek(probeFile, 0, SEEK_END);
        needsHeader = std::ftell(probeFile) == 0;
        std::fclose(probeFile);
    }

    std::FILE* reportFile = std::fopen(path.c_str(), "a");
    if (reportFile == nullptr) {
        std::fprintf(stderr, "fatal error: failed to open measurement report file: %s\n", path.c_str());
        std::abort();
    }
    if (needsHeader) {
        std::fprintf(reportFile, "%s\n", headerColumns.c_str());
    }
    std::fprintf(reportFile, "%s\n", valueColumns.c_str());
    std::fclose(reportFile);
}

void recordFrameTimingSamples(TimingStore& store, double currentTime, bool includeInReport,
                              const double* values)
{
    const float elapsedSeconds = static_cast<float>(currentTime - store.startTime);
    store.historyTimeSeconds.push_back(elapsedSeconds);

    for (int i = 0; i < store.itemCount; ++i) {
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
