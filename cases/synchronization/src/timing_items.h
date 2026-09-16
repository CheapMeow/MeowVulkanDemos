#pragma once

#include "timing.h"

// 本 case 的计时项。下标即计时项编号，顺序必须与各处填写计时数值的顺序一致
enum TimingId {
    TIMING_FRAME = 0,               // 帧时间：主循环相邻两次迭代之间经过的时间
    TIMING_CPU_RECORD_PRODUCER,     // 记录：生产者（子通道模式下为 0，生产者在消费者那一侧记录）
    TIMING_CPU_RECORD_SYNC,         // 记录：同步命令与生产者提交
    TIMING_CPU_RECORD_CONSUMER,     // 记录：消费者（渲染通道、界面之外的部分）
    TIMING_CPU_RECORD_UI,           // 记录：界面绘制命令
    TIMING_CPU_RECORD_CAPTURE,      // 记录：抓帧用的画面回读拷贝（未抓帧时为 0）
    TIMING_CPU_RECORD_SUBMIT,       // 记录：写入结束时间戳、结束命令缓冲、提交队列
    TIMING_CPU_WAIT_SYNC,           // 主机等待：围栏、队列空闲、设备空闲或查询时间线计数值
    TIMING_GPU_PRODUCER,            // 设备时间：生产者那一段
    TIMING_GPU_CONSUMER,            // 设备时间：消费者那一段

    TIMING_ID_COUNT
};

const TimingItemDescription* caseTimingItems();
