#pragma once

#include "timing.h"

// 本 case 的计时项。下标即计时项编号，顺序必须与各处填写计时数值的顺序一致
enum TimingId {
    TIMING_FRAME = 0,          // 帧时间：主循环相邻两次迭代之间经过的时间
    TIMING_CPU_RECORD_BEGIN,   // 记录：重置并开始命令缓冲、重置时间戳查询池
    TIMING_CPU_RECORD_PREPASS, // 记录：深度预通道
    TIMING_CPU_RECORD_CULL,    // 记录：遮挡剔除的计算派发
    TIMING_CPU_RECORD_PYRAMID, // 记录：金字塔构建
    TIMING_CPU_RECORD_SCENE,   // 记录：场景颜色通道
    TIMING_CPU_RECORD_PRESENT, // 记录：输出通道
    TIMING_CPU_RECORD_UI,      // 记录：界面绘制命令
    TIMING_CPU_RECORD_CAPTURE, // 记录：抓帧用的画面回读拷贝
    TIMING_CPU_RECORD_SUBMIT,  // 记录：结束命令缓冲、提交队列
    TIMING_GPU_TOTAL,          // 设备时间：四段设备时间之和
    TIMING_GPU_PREPASS,        // 设备时间：深度预通道
    TIMING_GPU_CULL,           // 设备时间：遮挡剔除
    TIMING_GPU_PYRAMID,        // 设备时间：金字塔构建
    TIMING_GPU_SCENE,          // 设备时间：场景颜色通道与输出

    TIMING_ID_COUNT
};

const TimingItemDescription* caseTimingItems();
