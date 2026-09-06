#pragma once

#include <vulkan/vulkan.h>

// 命令缓冲上的调试标记。RenderDoc、Android GPU Inspector 这类工具按这些名字分段显示，
// 直接给出每一段命令的耗时，因此剔除调度、G-Buffer 通道、光照通道这些段落都打上标记。
// VK_EXT_debug_utils 不是每个设备都有，取不到入口时标记就是空操作，不影响绘制
struct VulkanMarkers {
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
};

// 实例创建之后调用，按扩展是否可用取命令入口
void initVulkanMarkers(VkInstance instance, VulkanMarkers& outMarkers);

void beginCommandLabel(const VulkanMarkers& markers, VkCommandBuffer commandBuffer, const char* name);
void endCommandLabel(const VulkanMarkers& markers, VkCommandBuffer commandBuffer);
