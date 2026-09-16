#include "timing_items.h"

// 界面显示名称与报告列名，顺序与 TimingId 一致
static const TimingItemDescription CASE_TIMING_ITEMS[TIMING_ID_COUNT] = {
    { "帧时间", "frame_ms" },
    { "记录：命令缓冲起始", "cpu_record_begin_ms" },
    { "记录：场景通道", "cpu_record_scene_ms" },
    { "记录：景深通道", "cpu_record_post_ms" },
    { "记录：界面绘制", "cpu_record_ui_ms" },
    { "记录：抓帧拷贝", "cpu_record_capture_ms" },
    { "记录：提交命令", "cpu_record_submit_ms" },
    { "设备时间：整帧", "gpu_ms" },
    { "设备时间：场景通道", "gpu_scene_ms" },
    { "设备时间：景深通道", "gpu_post_ms" },
};

const TimingItemDescription* caseTimingItems()
{
    return CASE_TIMING_ITEMS;
}
