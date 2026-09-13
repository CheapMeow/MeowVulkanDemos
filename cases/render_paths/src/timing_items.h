#pragma once

#include "timing.h"

// 本 case 的计时项。下标即计时项编号，顺序必须与各处填写计时数值的顺序一致
enum TimingId {
    TIMING_FRAME = 0,              // 帧时间：主循环相邻两次迭代之间经过的时间
    TIMING_CPU_RECORD_BEGIN,       // 记录：重置并开始命令缓冲、重置时间戳查询池
    TIMING_CPU_RECORD_TILE,        // 记录：分块光源列表的清除与计算
    TIMING_CPU_RECORD_GEOMETRY,    // 记录：几何通道
    TIMING_CPU_RECORD_LIGHTING,    // 记录：延迟路径的光照通道
    TIMING_CPU_RECORD_PRESENT,     // 记录：输出通道
    TIMING_CPU_RECORD_UI,          // 记录：界面绘制命令
    TIMING_CPU_RECORD_CAPTURE,     // 记录：抓帧用的画面回读拷贝
    TIMING_CPU_RECORD_SUBMIT,      // 记录：写入结束时间戳、结束命令缓冲、提交队列
    TIMING_GPU_TOTAL,              // 设备时间：时间戳查询覆盖的整段 GPU 执行时间

    TIMING_ID_COUNT
};

const TimingItemDescription* caseTimingItems();
