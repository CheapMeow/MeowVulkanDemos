#include "timing_items.h"

// 界面显示名称与报告列名，顺序与 TimingId 一致
static const TimingItemDescription CASE_TIMING_ITEMS[TIMING_ID_COUNT] = {
    { "帧时间", "frame_ms" },
    { "记录：命令缓冲起始", "cpu_record_begin_ms" },
    { "记录：几何通道", "cpu_record_geometry_ms" },
    { "记录：金字塔构建", "cpu_record_pyramid_ms" },
    { "记录：反射通道", "cpu_record_reflect_ms" },
    { "记录：输出通道", "cpu_record_present_ms" },
    { "记录：界面绘制", "cpu_record_ui_ms" },
    { "记录：抓帧拷贝", "cpu_record_capture_ms" },
    { "记录：提交命令", "cpu_record_submit_ms" },
    { "设备时间", "gpu_ms" },
    { "设备：几何通道", "gpu_geometry_ms" },
    { "设备：金字塔构建", "gpu_pyramid_ms" },
    { "设备：反射通道", "gpu_reflect_ms" },
    { "设备：输出通道", "gpu_present_ms" },
};

const TimingItemDescription* caseTimingItems()
{
    return CASE_TIMING_ITEMS;
}
