#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 全部计时项。每一项对应一次独立的耗时测量，覆盖一帧里主机与设备两侧的每一个功能步骤
enum TimingId {
    TIMING_FRAME = 0,                  // 帧时间：主循环相邻两次迭代之间经过的时间
    TIMING_CPU_CULL,                   // 主机剔除：cullInstancesOnCpu 遍历全部实例的耗时
    TIMING_CPU_RECORD_BEGIN,           // 记录：重置并开始命令缓冲、重置时间戳查询池
    TIMING_CPU_RECORD_CULL_DISPATCH,   // 记录：indirect 路径的剔除计算调度（其余路径为 0）
    TIMING_CPU_RECORD_GBUFFER_PASS,    // 记录：G-Buffer 通道，包含该路径的全部绘制命令
    TIMING_CPU_RECORD_LIGHTING_PASS,   // 记录：光照通道，到全屏三角形绘制命令为止
    TIMING_CPU_RECORD_UI,              // 记录：界面绘制命令与光照通道结束
    TIMING_CPU_RECORD_CAPTURE,         // 记录：抓帧用的画面回读拷贝（未抓帧时为 0）
    TIMING_CPU_RECORD_SUBMIT,          // 记录：写入结束时间戳、结束命令缓冲、提交队列
    TIMING_GPU_TOTAL,                  // 设备时间：时间戳查询覆盖的整段 GPU 执行时间

    TIMING_ID_COUNT
};

const char* timingDisplayName(TimingId id);       // 界面与控制台使用的中文名称
const char* timingReportColumnName(TimingId id);   // CSV 表头使用的英文列名

// 单调时钟，单位秒，返回第一次调用以来经过的时间。两个平台共用同一套实现，
// 免得各处的耗时计算依赖窗口库
double nowSeconds();

// 最近多少帧参与界面上的均值与标准差计算
enum { TIMING_WINDOW_CAPACITY = 100 };

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
    double startTime;

    TimingWindow window[TIMING_ID_COUNT];
    int windowCursor;  // 下一次写入滑动窗口的下标，循环递增

    // 全部计时项共用同一条时间轴，因为它们在每一帧里同时采样
    std::vector<float> historyTimeSeconds;
    std::vector<float> historyValues[TIMING_ID_COUNT];

    TimingReportAccumulator report[TIMING_ID_COUNT];
};

void initTimingStore(TimingStore& store, double startTime);

// 清空滑动窗口的样本与统计量，历史曲线与测量报告不受影响。切换绘制路径时调用，
// 避免界面上的均值和标准差在切换后的一段时间内混杂新旧路径的数据
void resetTimingWindows(TimingStore& store);

// 清空测量报告的在线统计。TCP 控制的分段测量在 begin 时调用，把这一段与上一段分开
void resetTimingReport(TimingStore& store);

// 每帧调用一次，values 必须按 TimingId 的顺序填满全部计时项的毫秒数
// includeInReport 为真时，本帧同时计入测量报告的在线统计（用于跳过预热阶段）
void recordFrameTimingSamples(TimingStore& store, double currentTime, bool includeInReport,
                              const double values[TIMING_ID_COUNT]);

double timingReportStandardDeviation(const TimingReportAccumulator& accumulator);

// 按测量报告的表头生成一行数据，供分段测量与整段报告共用
std::string timingReportLine(const char* pathName, uint32_t instances, uint32_t visibleInstances,
                             uint32_t drawCommands, const TimingStore& store);
