#include "user_interface.h"

#include "vk_check.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <cstdio>

// 界面上的全部文本。字体字形范围、启动校验与界面绘制都以这份定义为唯一来源，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan Indirect Draw 对比";

static const char* const TEXT_SECTION_PATH = "绘制路径";
static const char* const TEXT_PATH_TRADITIONAL = "传统 drawIndexed（主机剔除，逐实例一条命令）";
static const char* const TEXT_PATH_INDIRECT = "indirect（计算着色器剔除，一条命令）";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_LIGHT_COUNT = "光源数量";
static const char* const TEXT_FAR_PLANE = "远裁剪面";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_TIMING = "本帧耗时";
static const char* const TEXT_FRAME_TIME = "帧时间       %7.3f ms  (%.0f FPS)";
static const char* const TEXT_CPU_CULL_TIME = "主机剔除     %7.3f ms";
static const char* const TEXT_CPU_RECORD_TIME = "主机记录命令 %7.3f ms";
static const char* const TEXT_GPU_TIME = "设备时间     %7.3f ms";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_TOTAL_INSTANCES = "实例总数 %d";
static const char* const TEXT_VISIBLE_INSTANCES = "可见实例 %u";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_SWITCH = "空格键切换绘制路径，效果与上面的单选按钮相同";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "两条路径共用同一份着色器与同一套剔除判据，画面完全一致。"
    "把实例数量或者远裁剪面调大，观察主机剔除与主机记录命令这两项的变化，"
    "设备时间在两条路径上保持一致。";

static const char* const ALL_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_PATH,       TEXT_PATH_TRADITIONAL, TEXT_PATH_INDIRECT,
    TEXT_SECTION_SCENE,    TEXT_INSTANCE_COUNT,     TEXT_LIGHT_COUNT,      TEXT_FAR_PLANE,
    TEXT_MOVE_SPEED,       TEXT_SECTION_TIMING,     TEXT_FRAME_TIME,       TEXT_CPU_CULL_TIME,
    TEXT_CPU_RECORD_TIME,  TEXT_GPU_TIME,           TEXT_SECTION_WORKLOAD, TEXT_TOTAL_INSTANCES,
    TEXT_VISIBLE_INSTANCES, TEXT_DRAW_COMMANDS,     TEXT_SECTION_GUIDE,    TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,       TEXT_GUIDE_SWITCH,       TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,
    TEXT_EXPLANATION
};

enum { INTERFACE_TEXT_COUNT = sizeof(ALL_INTERFACE_TEXTS) / sizeof(ALL_INTERFACE_TEXTS[0]) };

// 字形范围需要在字体图集构建期间保持有效
static ImVector<ImWchar> gGlyphRanges;

static void checkImGuiResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        FATAL("imgui 的 Vulkan 调用失败");
    }
}

// 界面文本里超出基本拉丁字母范围的字符统计
struct GlyphUsage {
    int ideographCount;    // 汉字
    int punctuationCount;  // 中日韩标点与全角符号
};

// 缺少字形的字符会被替换成问号，直接在启动时判定为失败
static GlyphUsage verifyGlyphsPresent(ImFont* font)
{
    GlyphUsage usage = {};

    for (int i = 0; i < INTERFACE_TEXT_COUNT; ++i) {
        const char* cursor = ALL_INTERFACE_TEXTS[i];
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

            if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
                ++usage.ideographCount;
            } else if ((codepoint >= 0x3000 && codepoint <= 0x303F) ||
                       (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) {
                ++usage.punctuationCount;
            }
        }
    }

    return usage;
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

    // 默认字体没有汉字，使用系统自带的微软雅黑，字形范围由界面文本本身决定
    ImFontGlyphRangesBuilder rangesBuilder;
    rangesBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (int i = 0; i < INTERFACE_TEXT_COUNT; ++i) {
        rangesBuilder.AddText(ALL_INTERFACE_TEXTS[i]);
    }
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

    const GlyphUsage usage = verifyGlyphsPresent(font);
    std::printf("界面字体 msyh.ttc: 载入字形 %d 个, 界面文本用到汉字 %d 处与全角标点 %d 处, 全部命中\n",
                font->Glyphs.Size, usage.ideographCount, usage.punctuationCount);
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
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_PATH);
    int selectedPath = state.drawPath == DRAW_PATH_TRADITIONAL ? 0 : 1;
    ImGui::RadioButton(TEXT_PATH_TRADITIONAL, &selectedPath, 0);
    ImGui::RadioButton(TEXT_PATH_INDIRECT, &selectedPath, 1);
    state.drawPath = selectedPath == 0 ? DRAW_PATH_TRADITIONAL : DRAW_PATH_INDIRECT;

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_INSTANCE_COUNT, &state.activeInstanceCount, 1, maxInstanceCount, "%d",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt(TEXT_LIGHT_COUNT, &state.activeLightCount, 1, maxLightCount);
    ImGui::SliderFloat(TEXT_FAR_PLANE, &state.farPlane, 40.0f, 900.0f, "%.0f");
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 5.0f, 400.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_TIMING);
    ImGui::Text(TEXT_FRAME_TIME, statistics.frameMilliseconds,
                statistics.frameMilliseconds > 0.0 ? 1000.0 / statistics.frameMilliseconds : 0.0);
    ImGui::Text(TEXT_CPU_CULL_TIME, statistics.cpuCullMilliseconds);
    ImGui::Text(TEXT_CPU_RECORD_TIME, statistics.cpuRecordMilliseconds);
    ImGui::Text(TEXT_GPU_TIME, statistics.gpuMilliseconds);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_TOTAL_INSTANCES, state.activeInstanceCount);
    ImGui::Text(TEXT_VISIBLE_INSTANCES, statistics.visibleInstanceCount);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_SWITCH);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

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
