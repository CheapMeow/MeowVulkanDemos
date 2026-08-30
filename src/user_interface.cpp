#include "user_interface.h"

#include "vk_check.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

// 界面文本用到的全部汉字，字体只加载这些字形，启动时逐个确认字体里确实有对应字形
static const char* INTERFACE_GLYPHS =
    "绘制路径传统主机剔除逐实例一条命令计算着色器场景数量光源远裁剪面移动速度本帧耗时间记录设备工作总可见操作"
    "指南前后左右下降上升方向键转动视角按住加四倍空格切换效果与的单选按钮相同退出程序拖标题栏以板两共用份套判"
    "据画完全一致把或者调大观察项变化在保持对比";

// 字形范围需要在字体图集构建期间保持有效
static ImVector<ImWchar> gGlyphRanges;

static void checkImGuiResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        FATAL("imgui 的 Vulkan 调用失败");
    }
}

// 缺少字形会让界面显示成空白方块，直接在启动时判定为失败
static void verifyGlyphsPresent(ImFont* font, const char* text)
{
    const char* cursor = text;
    while (*cursor != '\0') {
        unsigned int codepoint = 0;
        const int consumedBytes = ImTextCharFromUtf8(&codepoint, cursor, nullptr);
        if (consumedBytes == 0) {
            FATAL("界面文本的字符编码无法解析");
        }
        cursor += consumedBytes;
        if (font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) == nullptr) {
            FATAL("字体缺少界面文本需要的字形, 码点 U+%04X", codepoint);
        }
    }
}

void createUserInterface(const VulkanContext& ctx, const Renderer& renderer, UserInterface& ui)
{
    // 界面只需要采样字体图集，一个组合图像采样器就够
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 8;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &ui.descriptorPool));

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 默认字体没有汉字，使用系统自带的微软雅黑，只加载界面实际用到的字形
    ImFontGlyphRangesBuilder rangesBuilder;
    rangesBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    rangesBuilder.AddText(INTERFACE_GLYPHS);
    rangesBuilder.BuildRanges(&gGlyphRanges);

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    ImFont* font =
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18.0f, &fontConfig, gGlyphRanges.Data);
    if (font == nullptr) {
        FATAL("加载字体 C:/Windows/Fonts/msyh.ttc 失败");
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);

    if (!ImGui_ImplGlfw_InitForVulkan(ctx.window, true)) {
        FATAL("初始化 imgui 的 GLFW 后端失败");
    }

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = VK_API_VERSION_1_2;
    initInfo.Instance = ctx.instance;
    initInfo.PhysicalDevice = ctx.physicalDevice;
    initInfo.Device = ctx.device;
    initInfo.QueueFamily = ctx.queueFamilyIndex;
    initInfo.Queue = ctx.queue;
    initInfo.DescriptorPool = ui.descriptorPool;
    initInfo.RenderPass = renderer.lightingRenderPass;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = ctx.swapchainImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Subpass = 0;
    initInfo.CheckVkResultFn = checkImGuiResult;
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        FATAL("初始化 imgui 的 Vulkan 后端失败");
    }
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        FATAL("创建 imgui 字体纹理失败");
    }

    verifyGlyphsPresent(font, INTERFACE_GLYPHS);
}

void destroyUserInterface(const VulkanContext& ctx, UserInterface& ui)
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(ctx.device, ui.descriptorPool, nullptr);
    ui.descriptorPool = VK_NULL_HANDLE;
}

void beginUserInterfaceFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, int maxInstanceCount, int maxLightCount)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Vulkan Indirect Draw 对比");

    ImGui::SeparatorText("绘制路径");
    int selectedPath = state.drawPath == DRAW_PATH_TRADITIONAL ? 0 : 1;
    ImGui::RadioButton("传统 drawIndexed（主机剔除，逐实例一条命令）", &selectedPath, 0);
    ImGui::RadioButton("indirect（计算着色器剔除，一条命令）", &selectedPath, 1);
    state.drawPath = selectedPath == 0 ? DRAW_PATH_TRADITIONAL : DRAW_PATH_INDIRECT;

    ImGui::SeparatorText("场景");
    ImGui::SliderInt("实例数量", &state.activeInstanceCount, 1, maxInstanceCount, "%d",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt("光源数量", &state.activeLightCount, 1, maxLightCount);
    ImGui::SliderFloat("远裁剪面", &state.farPlane, 40.0f, 900.0f, "%.0f");
    ImGui::SliderFloat("移动速度", &state.cameraMoveSpeed, 5.0f, 400.0f, "%.0f");

    ImGui::SeparatorText("本帧耗时");
    ImGui::Text("帧时间       %7.3f ms  (%.0f FPS)", statistics.frameMilliseconds,
                statistics.frameMilliseconds > 0.0 ? 1000.0 / statistics.frameMilliseconds : 0.0);
    ImGui::Text("主机剔除     %7.3f ms", statistics.cpuCullMilliseconds);
    ImGui::Text("主机记录命令 %7.3f ms", statistics.cpuRecordMilliseconds);
    ImGui::Text("设备时间     %7.3f ms", statistics.gpuMilliseconds);

    ImGui::SeparatorText("本帧工作量");
    ImGui::Text("实例总数 %d", state.activeInstanceCount);
    ImGui::Text("可见实例 %u", statistics.visibleInstanceCount);
    ImGui::Text("绘制命令 %u 条", statistics.drawCallCount);

    ImGui::SeparatorText("操作指南");
    ImGui::BulletText("W A S D 前后左右移动，Q 下降，E 上升");
    ImGui::BulletText("方向键转动视角，按住左 Shift 加速四倍");
    ImGui::BulletText("空格键切换绘制路径，效果与上面的单选按钮相同");
    ImGui::BulletText("Esc 退出程序");
    ImGui::BulletText("拖动标题栏可以移动本面板");

    ImGui::Spacing();
    ImGui::TextWrapped("两条路径共用同一份着色器与同一套剔除判据，画面完全一致。"
                       "把实例数量或者远裁剪面调大，观察主机剔除与主机记录命令这两项的变化，"
                       "设备时间在两条路径上保持一致。");

    ImGui::End();
}

void endUserInterfaceFrame()
{
    ImGui::Render();
}

bool userInterfaceWantsKeyboard()
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

void recordUserInterfaceCommands(VkCommandBuffer commandBuffer)
{
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}
