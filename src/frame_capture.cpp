#include "frame_capture.h"

#include "vk_check.h"

#include <stb_image_write.h>

#include <cstdio>
#include <vector>

void createCaptureBuffer(const VulkanContext& ctx, GpuBuffer& outBuffer)
{
    const VkDeviceSize byteCount =
        static_cast<VkDeviceSize>(ctx.swapchainExtent.width) * ctx.swapchainExtent.height * 4;
    createBuffer(ctx, byteCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, outBuffer);
}

void writeCaptureBufferToPng(const VulkanContext& ctx, const GpuBuffer& buffer, const std::string& pngPath)
{
    const uint32_t width = ctx.swapchainExtent.width;
    const uint32_t height = ctx.swapchainExtent.height;
    const size_t pixelCount = static_cast<size_t>(width) * height;

    const unsigned char* sourcePixels = static_cast<const unsigned char*>(buffer.mapped);
    std::vector<unsigned char> rgbaPixels(pixelCount * 4);

    const bool sourceIsBgra = ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                              ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;

    double luminanceSum = 0.0;
    double geometryLuminanceSum = 0.0;
    uint32_t geometryPixelCount = 0;
    unsigned char maximumChannel = 0;

    // 光照通道对没有几何的像素写入固定背景色，据此统计几何覆盖率
    const unsigned char backgroundRed = static_cast<unsigned char>(0.02f * 255.0f + 0.5f);
    const unsigned char backgroundGreen = static_cast<unsigned char>(0.025f * 255.0f + 0.5f);
    const unsigned char backgroundBlue = static_cast<unsigned char>(0.035f * 255.0f + 0.5f);

    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const unsigned char channel0 = sourcePixels[pixel * 4 + 0];
        const unsigned char channel1 = sourcePixels[pixel * 4 + 1];
        const unsigned char channel2 = sourcePixels[pixel * 4 + 2];

        const unsigned char red = sourceIsBgra ? channel2 : channel0;
        const unsigned char green = channel1;
        const unsigned char blue = sourceIsBgra ? channel0 : channel2;

        rgbaPixels[pixel * 4 + 0] = red;
        rgbaPixels[pixel * 4 + 1] = green;
        rgbaPixels[pixel * 4 + 2] = blue;
        rgbaPixels[pixel * 4 + 3] = 255;

        const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
        luminanceSum += luminance;

        const bool isBackground = red == backgroundRed && green == backgroundGreen && blue == backgroundBlue;
        if (!isBackground) {
            ++geometryPixelCount;
            geometryLuminanceSum += luminance;
        }

        if (red > maximumChannel) {
            maximumChannel = red;
        }
        if (green > maximumChannel) {
            maximumChannel = green;
        }
        if (blue > maximumChannel) {
            maximumChannel = blue;
        }
    }

    const double geometryAverageLuminance =
        geometryPixelCount > 0 ? geometryLuminanceSum / static_cast<double>(geometryPixelCount) : 0.0;

    std::printf("抓取画面 %s: 几何覆盖率 %.2f%%, 几何像素平均亮度 %.2f, 全图平均亮度 %.2f, 最大通道值 %u\n",
                pngPath.c_str(),
                100.0 * static_cast<double>(geometryPixelCount) / static_cast<double>(pixelCount),
                geometryAverageLuminance, luminanceSum / static_cast<double>(pixelCount),
                static_cast<unsigned>(maximumChannel));

    if (stbi_write_png(pngPath.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgbaPixels.data(),
                       static_cast<int>(width) * 4) == 0) {
        FATAL("写出 PNG 文件 %s 失败", pngPath.c_str());
    }
}
