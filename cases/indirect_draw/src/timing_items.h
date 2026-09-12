#pragma once

#include "timing.h"

// 本 case 的计时项。下标即计时项编号，顺序必须与各处填写计时数值的顺序一致
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

const TimingItemDescription* caseTimingItems();
