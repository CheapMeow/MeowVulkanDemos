#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 每个 case 用一张表描述自己的一帧拆成了哪几个计时项，表的下标就是计时项编号。
// 每一项同时给出界面显示名称与报告列名，新增计时项只需要改这张表
struct TimingItemDescription {
    const char* displayName;   // 界面与控制台使用的中文名称
    const char* reportColumn;  // CSV 表头使用的英文列名
};

// 一帧最多拆成这么多项
enum { TIMING_MAX_ITEMS = 16 };

// 最近多少帧参与界面上的均值与标准差计算
enum { TIMING_WINDOW_CAPACITY = 100 };

// 单调时钟，单位秒，返回第一次调用以来经过的时间。两个平台共用同一套实现，
// 免得各处的耗时计算依赖窗口库
double nowSeconds();

// 单个计时项在滑动窗口内的样本与统计量
struct TimingWindow {
    double samples[TIMING_WINDOW_CAPACITY];
    int sampleCount;    // 已写入的样本数，达到窗口容量后不再增长
    double mean;
    double standardDeviation;
};

// 单个计时项从预热结束到当前的在线统计量，使用 Welford 算法逐帧累加
struct TimingReportAccumulator {
    uint64_t sampleCount;
    double mean;
    double sumSquaredDelta;  // Welford 算法里的 M2，方差为 sumSquaredDelta / (sampleCount - 1)
};

// 全部计时项的存储：滑动窗口统计、从启动到现在的完整历史曲线、测量报告的在线统计
struct TimingStore {
    const TimingItemDescription* items;
    int itemCount;

    double startTime;

    TimingWindow window[TIMING_MAX_ITEMS];
    int windowCursor;  // 下一次写入滑动窗口的下标，循环递增

    // 全部计时项共用同一条时间轴，因为它们在每一帧里同时采样
    std::vector<float> historyTimeSeconds;
    std::vector<float> historyValues[TIMING_MAX_ITEMS];

    TimingReportAccumulator report[TIMING_MAX_ITEMS];
};

// items 指向 case 自己定义的描述表，生命周期必须覆盖 TimingStore 的全部使用时间
void initTimingStore(TimingStore& store, const TimingItemDescription* items, int itemCount, double startTime);

// 清空滑动窗口的样本与统计量，历史曲线与测量报告不受影响。切换配置时调用，
// 避免界面上的均值和标准差在切换后的一段时间内混杂新旧配置的数据
void resetTimingWindows(TimingStore& store);

// 清空测量报告的在线统计。TCP 控制的分段测量在 begin 时调用，把这一段与上一段分开
void resetTimingReport(TimingStore& store);

// 每帧调用一次，values 必须按 items 的顺序填满 itemCount 项计时项的毫秒数
// includeInReport 为真时，本帧同时计入测量报告的在线统计（用于跳过预热阶段）
void recordFrameTimingSamples(TimingStore& store, double currentTime, bool includeInReport,
                              const double* values);

double timingReportStandardDeviation(const TimingReportAccumulator& accumulator);

// 报告表头里计时列的名字，以及一行数据里计时列的数值。case 自己的前几列由调用方拼接
std::string timingReportHeaderColumns(const TimingStore& store);
std::string timingReportValueColumns(const TimingStore& store);

// 把一行追加到报告文件，文件为空时先写一行表头。报告由程序自己写入，不依赖控制台重定向
void appendMeasurementReport(const std::string& path, const std::string& headerColumns,
                             const std::string& valueColumns);
