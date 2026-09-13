#include "timing_items.h"

// 界面显示名称与报告列名，顺序与 TimingId 一致
static const TimingItemDescription CASE_TIMING_ITEMS[TIMING_ID_COUNT] = {
    { "帧时间", "frame_ms" },
    { "记录：命令缓冲起始", "cpu_record_begin_ms" },
    { "记录：计算派发", "cpu_record_dispatch_ms" },
    { "记录：屏障与显示", "cpu_record_composite_ms" },
    { "记录：界面绘制", "cpu_record_ui_ms" },
    { "记录：抓帧拷贝", "cpu_record_capture_ms" },
    { "记录：提交命令", "cpu_record_submit_ms" },
    { "设备时间", "gpu_ms" },
};

const TimingItemDescription* caseTimingItems()
{
    return CASE_TIMING_ITEMS;
}
