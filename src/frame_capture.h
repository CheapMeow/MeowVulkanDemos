#pragma once

#include "vk_context.h"
#include "vk_resources.h"

#include <string>

void createCaptureBuffer(const VulkanContext& ctx, GpuBuffer& outBuffer);
// 把抓取缓冲的内容写成 PNG，并打印像素统计
void writeCaptureBufferToPng(const VulkanContext& ctx, const GpuBuffer& buffer, const std::string& pngPath);
