#pragma once

#include "timing.h"

// 本 case 的计时项。下标即计时项编号，顺序必须与各处填写计时数值的顺序一致
enum TimingId {
    TIMING_FRAME = 0,         // 帧时间：主循环相邻两次迭代之间经过的时间
    TIMING_CPU_RECORD_BEGIN,  // 记录：重置并开始命令缓冲、重置时间戳查询池
    TIMING_CPU_RECORD_SCENE,  // 记录：场景通道的背景、球与参考块
    TIMING_CPU_RECORD_GRADE,  // 记录：调色通道与界面绘制命令
    TIMING_CPU_RECORD_UI,     // 记录：界面绘制命令
    TIMING_CPU_RECORD_CAPTURE,// 记录：抓帧用的画面回读拷贝（未抓帧时为 0）
    TIMING_CPU_RECORD_SUBMIT, // 记录：写入结束时间戳、结束命令缓冲、提交队列
    TIMING_GPU_TOTAL,         // 设备时间：整帧的时间戳跨度
    TIMING_GPU_SCENE,         // 设备时间：场景通道
    TIMING_GPU_GRADE,         // 设备时间：调色通道到帧结束

    TIMING_ID_COUNT
};

const TimingItemDescription* caseTimingItems();
