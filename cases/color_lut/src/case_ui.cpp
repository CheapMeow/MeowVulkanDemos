#include "case_ui.h"

#include "renderer.h"
#include "scene_setup.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 颜色查找表";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_LIGHT_INTENSITY = "平行光强度";
static const char* const TEXT_AMBIENT = "环境项强度";
static const char* const TEXT_EXPOSURE = "曝光倍数";

static const char* const TEXT_SECTION_GRADE = "调色";
static const char* const TEXT_MODE = "处理方式";
static const char* const TEXT_TABLE_SIZE = "三维表格尺寸";

static const char* const MODE_LABELS[] = { "不做处理", "三维查找表", "每通道一维曲线", "解析变换" };
enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_SCENE_HINT =
    "七个彩球加一段渐变背景画进一张半精度浮点的高动态范围图像，之后整幅画面过一遍色调映射与"
    "输出编码，再按所选方式调色。屏幕底部左边是十六级灰阶、右边是八块饱和色：灰阶上四条路径"
    "必然一致，饱和色上每通道一维曲线会露馅——它只能按分量各自调整，做不到与色相相关的变换。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "查找表把一段昂贵的逐像素计算换成几次纹理读取：三维表把颜色空间切成格子，每个格子记下"
    "调色之后的值，运行时按三个分量定位并做三线性插值。格子越密误差越小，代价是纹理越大。"
    "每通道一维曲线更省，但它假设三个分量互不影响，色温、去饱和这类与亮度或色相相关的变换"
    "它就表达不了。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_SCENE,  TEXT_MOVE_SPEED,    TEXT_LIGHT_INTENSITY,
    TEXT_AMBIENT,         TEXT_EXPOSURE,       TEXT_SECTION_GRADE, TEXT_MODE,
    MODE_LABELS[0],       MODE_LABELS[1],      MODE_LABELS[2],     MODE_LABELS[3],
    TEXT_TABLE_SIZE,      TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS, TEXT_RENDER_PASSES,
    TEXT_SCENE_HINT,      TEXT_SECTION_GUIDE,  TEXT_GUIDE_MOVE,    TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,      TEXT_GUIDE_DRAG,     TEXT_EXPLANATION,
};

enum { CASE_INTERFACE_TEXT_COUNT = sizeof(CASE_INTERFACE_TEXTS) / sizeof(CASE_INTERFACE_TEXTS[0]) };

const char* const* caseInterfaceTexts(int& outCount)
{
    // 计时项的显示名也属于界面文本，一并交给字形范围
    static std::vector<const char*> texts;
    if (texts.empty()) {
        for (int i = 0; i < CASE_INTERFACE_TEXT_COUNT; ++i) {
            texts.push_back(CASE_INTERFACE_TEXTS[i]);
        }
        const TimingItemDescription* items = caseTimingItems();
        for (int i = 0; i < TIMING_ID_COUNT; ++i) {
            texts.push_back(items[i].displayName);
        }
    }
    outCount = static_cast<int>(texts.size());
    return texts.data();
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(580.0f, 940.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat(TEXT_LIGHT_INTENSITY, &state.lightIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat(TEXT_AMBIENT, &state.ambient, 0.0f, 0.5f, "%.3f");
    ImGui::SliderFloat(TEXT_EXPOSURE, &state.exposure, 0.1f, 8.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_GRADE);
    int mode = static_cast<int>(state.gradeMode);
    if (ImGui::Combo(TEXT_MODE, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.gradeMode = static_cast<uint32_t>(mode);
    }
    int size = static_cast<int>(state.lutSize);
    if (ImGui::SliderInt(TEXT_TABLE_SIZE, &size, GRADE_LUT_MIN_SIZE, GRADE_LUT_MAX_SIZE)) {
        state.lutSize = static_cast<uint32_t>(size);
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_RENDER_PASSES, statistics.renderPassCount);
    ImGui::TextWrapped("%s", TEXT_SCENE_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
