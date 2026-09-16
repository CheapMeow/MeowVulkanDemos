# draw_cost：绘制命令的固定成本与状态排序

构建方式见仓库根目录的 README。

## 简介

一批在屏幕上只有几个像素的小方块，半边长 0.02（[`src/renderer.cpp:43-45`](src/renderer.cpp#L43-L45)），用低差异序列铺在相机前的平面上（
[`src/renderer.cpp:22-33`](src/renderer.cpp#L22-L33)），任意实例数量下都覆盖同一块区域。每个方块一条绘制命令（
[`src/renderer.cpp:635`](src/renderer.cpp#L635)），方块本身几乎不着色（
[`shaders/small.frag:12-13`](shaders/small.frag#L12-L13)），因此设备时间随命令条数的变化就是每条命令的固定成本。

材质用一组颜色表示，每种材质一套常量缓冲与一个描述符集（[`src/renderer.cpp:395-412`](src/renderer.cpp#L395-L412)），对应真实场景里换一整套材质绑定（
[`shaders/draw_common.glsl:24-27`](shaders/draw_common.glsl#L24-L27)）。界面上的开关：

| 控件 | 作用 |
| --- | --- |
| 实例数量 | 也就是绘制命令条数（[`src/renderer.cpp:641`](src/renderer.cpp#L641)） |
| 材质数量 | 材质种类数，最多 8 种（[`src/renderer.cpp:17-18`](src/renderer.cpp#L17-L18)） |
| 提交顺序 | 按材质分组、两种材质交替、乱序（[`src/renderer.cpp:328-348`](src/renderer.cpp#L328-L348)） |
| 每条命令前重复绑定 | 每条命令前重新绑定同一套描述符集与顶点缓冲（[`src/renderer.cpp:621-627`](src/renderer.cpp#L621-L627)） |
| 渲染通道段数 | 把绘制拆进多段渲染通道（[`src/renderer.cpp:589-607`](src/renderer.cpp#L589-L607)） |

按材质分组时同一种材质只绑定一次材质描述符集，交替顺序每一条命令都要换，乱序介于两者之间（[`src/renderer.cpp:628-633`](src/renderer.cpp#L628-L633)）。

## 渲染流程

```mermaid
graph LR
    A[小方块实例] --> B[按提交顺序排出的绘制列表]
    B --> C[逐条 vkCmdDrawIndexed]
    C --> D[交换链图像]
```

第一段渲染通道清除颜色与深度附件（[`src/renderer.cpp:104-108`](src/renderer.cpp#L104-L108)），后续段的渲染通道加载已有内容（
[`src/renderer.cpp:110-114`](src/renderer.cpp#L110-L114)），两者的附件格式与最后布局一致，颜色附件一直留在颜色附件布局（
[`src/renderer.cpp:56-69`](src/renderer.cpp#L56-L69)）；全部绘制结束后用一次图像屏障把它转到呈现布局（
[`src/renderer.cpp:660-674`](src/renderer.cpp#L660-L674)）。

## 实现要点

### 每条命令的固定成本

一条绘制命令的代价里有一部分与画面内容无关：命令解码、管线与绑定重配置、流水线气泡、渲染通道边界。绘制列表按提交顺序逐条取出实例编号，每条实例提交一次 `vkCmdDrawIndexed`
（[`src/renderer.cpp:616-619`](src/renderer.cpp#L616-L619)、[`src/renderer.cpp:635`](src/renderer.cpp#L635)
），这些成本按命令条数累加；命令覆盖的像素越少，这部分在总耗时里的占比越高。本 case 的方块每个只有几十个像素，设备时间几乎全部由固定成本构成。

### 状态排序

同一种材质对应同一套描述符集（[`src/renderer.cpp:408-411`](src/renderer.cpp#L408-L411)），绑定一次之后连续绘制同材质的物体，材质编号不变就跳过重新绑定（
[`src/renderer.cpp:628-633`](src/renderer.cpp#L628-L633)）；交替顺序则每条命令都要换一次描述符集，驱动要在命令流里插入重配置。
分组与交替的差值就是状态重配置的成本。界面上的"每条命令前重复绑定"跳过这两处比较，每条命令都把场景描述符集、顶点缓冲、索引缓冲与材质描述符集重新发送一遍（
[`src/renderer.cpp:621-627`](src/renderer.cpp#L621-L627)）。

### 渲染通道边界

每一段边界都结束一次渲染通道再开下一次（[`src/renderer.cpp:596-607`](src/renderer.cpp#L596-L607)
、[`src/renderer.cpp:638`](src/renderer.cpp#L638)），在分块架构上是一次真实的搬运与一次流水线清空。把同一次绘制拆进多段渲染通道，段数越多越慢。
切换段之后上一段绑定的状态不再有效，场景描述符集与材质描述符集都要重新发送（[`src/renderer.cpp:612-614`](src/renderer.cpp#L612-L614)）。

## 界面控件

| 控件 | 作用 |
| --- | --- |
| 实例数量 | 绘制命令条数，上限 4096 |
| 材质数量 | 材质种类数，1 到 8 |
| 移动速度 | 相机移动速度 |
| 提交顺序 | 按材质分组、两种材质交替、乱序 |
| 每条命令前重复绑定 | 重复绑定同一套描述符集与顶点缓冲 |
| 渲染通道段数 | 1 到 16 |
| GPU 锁频 | 与间接绘制 case 共用同一个面板 |

面板同时显示绘制命令条数与场景说明，另附操作指南；耗时面板按本 case 的阶段拆分逐项列出。

## 命令行参数

命令行参数与界面控制同一套状态，用于自动化测试：

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--instances N` | 启动时的实例数量，也就是绘制命令条数 | 1024 |
| `--materials N` | 材质种类数，1 到 8 | 2 |
| `--order 名字` | 提交顺序，取 `grouped`、`alternating` 或 `random` | grouped |
| `--redundant-bind` | 每条命令前重复绑定 | 关闭 |
| `--pass-split N` | 渲染通道段数，1 到 16 | 1 |
| `--no-interface` | 不显示界面面板 | 显示 |
| `--auto-exit S` | 运行 S 秒后自动退出 | 关闭 |
| `--capture 文件名` | 退出前把画面写成 PNG 并打印像素统计 | 关闭 |
| `--report 文件名` | 退出前把本次运行的平均耗时以 CSV 形式追加到该文件 | 关闭 |
| `--control-port N` | 打开 TCP 控制服务并监听该端口 | 关闭 |
| `--core-clock N` | 启动时锁定的核心频率，单位 MHz，取最接近的可选档位 | 不锁频 |
| `--memory-clock N` | 启动时锁定的显存频率，单位 MHz，取最接近的可选档位 | 不锁频 |

键盘操作：`W` `A` `S` `D` 前后左右移动，`Q` `E` 下降与上升，方向键转动视角，左 Shift 加速，Esc 退出。

## 测试方法

```
build\meow_draw_cost.exe --auto-exit 5 --no-interface --instances 1024 --report intermediate\draw_cost.csv --order grouped
build\meow_draw_cost.exe --auto-exit 5 --no-interface --instances 1024 --report intermediate\draw_cost.csv --redundant-bind
```

本机 NVIDIA 显卡上的实测（设备时间均值）：

| 实例数量 | 提交顺序 | 重复绑定 | 渲染通道段数 | 设备时间 |
| --- | --- | --- | --- | --- |
| 512 | 按材质分组 | 关 | 1 | 0.012 ms |
| 1024 | 按材质分组 | 关 | 1 | 0.018 ms |
| 2048 | 按材质分组 | 关 | 1 | 0.035 ms |
| 1024 | 两种材质交替 | 关 | 1 | 0.018 ms |
| 1024 | 乱序 | 关 | 1 | 0.022 ms |
| 1024 | 按材质分组 | 开 | 1 | 0.028 ms |
| 1024 | 按材质分组 | 关 | 8 | 0.040 ms |

设备时间随命令条数近似线性，512 到 2048 之间的斜率约为每条命令 0.015 微秒。每条命令前重复绑定把 1024 条的耗时从 0.018 抬到 0.028，说明绑定重配置确实按命令条数累加。
把绘制拆成 8 段渲染通道后抬到 0.040，段边界的代价比重复绑定更大。分组与交替在本机没有可测差别，乱序略慢，这与驱动的命令缓冲合并能力有关：现成的描述符集在命令流里切换的代价已经被压得很低。

参数可以在同一次运行里改变：

```
adb -s <serial> forward tcp:21000 tcp:21000
```

桌面端用 `--control-port 21000` 启动后，连接并逐行发命令：`instances`/`materials`/`order`/`redundant-bind`/`pass-split` 改配置，
`begin` 与 `end` 圈定一段测量（`end` 返回一行与 CSV 同格式的数据），`quit` 退出。

随时间变化的参数曲线与测量报告格式与间接绘制 case 一致，报告里的前几列是提交顺序、材质数量、重复绑定开关、渲染通道段数与绘制命令条数。

## 源码结构

本 case 自己的文件都在 `cases/draw_cost` 下：

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、ANativeWindow 表面与交换链、主循环 |
| `src/renderer.cpp` | 两个渲染通道、材质描述符集、绘制列表重排、时间戳查询 |
| `src/scene_setup.cpp` | 面向相机的小方块网格 |
| `src/case_ui.cpp` | 本 case 的控制面板与全部界面文本 |
| `src/timing_items.cpp` | 本 case 的计时项定义 |
| `shaders/small.vert` | 按顺序缓冲取实例、变换到裁剪空间 |
| `shaders/small.frag` | 用材质颜色填充 |
| `shaders/draw_common.glsl` | 实例数据、顺序缓冲与材质常量的声明 |

## 安卓端差异

- 实例与设备申请 Vulkan 1.1，着色器以 `--target-env=vulkan1.1` 编译，覆盖只支持 1.1 的设备；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 设备时间只在设备支持时间戳时测量：驱动的 `timestampComputeAndGraphics` 能力与图形队列族的 `timestampValidBits` 都满足才创建查询池并记录时间戳，
  不支持的设备上"设备时间"恒为零，主机侧各项计时照常。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 相机固定在初始化位置，没有键盘输入；触摸事件交给 ImGui 的安卓后端，面板上的滑块和按钮可以直接操作。
- 帧率上限 60 FPS，避免无界空转发热。

### 安卓测试方法

adb 的目标设备由设备序列号指定，序列号用 `adb devices` 查询。只连接一台设备时命令里的 `-s <serial>` 可以省略。构建出 APK 后按下面方式安装并查看日志：

```
adb -s <serial> install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s <serial> logcat -s MeowVulkanDemo
```

应用内置回环 TCP 控制服务（桌面与安卓一致，端口 21000），安卓端经 `adb forward tcp:21000 tcp:21000` 把设备上的控制端口映射到本机，命令与桌面端相同。
