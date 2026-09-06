#include "vk_marker.h"

#include <cstring>

void initVulkanMarkers(VkInstance instance, VulkanMarkers& outMarkers)
{
    outMarkers.beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
    outMarkers.endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));
}

void beginCommandLabel(const VulkanMarkers& markers, VkCommandBuffer commandBuffer, const char* name)
{
    if (markers.beginLabel == nullptr) {
        return;
    }

    VkDebugUtilsLabelEXT label = {};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    markers.beginLabel(commandBuffer, &label);
}

void endCommandLabel(const VulkanMarkers& markers, VkCommandBuffer commandBuffer)
{
    if (markers.endLabel == nullptr) {
        return;
    }

    markers.endLabel(commandBuffer);
}
