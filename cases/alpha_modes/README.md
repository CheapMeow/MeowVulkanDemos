# alpha_modes：alpha test、alpha blend 与 alpha to coverage 的边缘对照

构建方式见仓库根目录的 README。

## 简介

一片程序生成的镂空植被，由一批相互交叠的卡片组成，每张卡片上有一个草丛掩码
（[`shaders/shape.frag:30-48`](shaders/shape.frag#L30-L48)）。植被围着自己的中心缓慢旋转，同一个掩码
在三种处理方式下呈现出不同的边界。

| 处理方式 | 做法 | 代价 |
| --- | --- | --- |
| alpha test | 片元着色器里 `discard` 丢掉低于阈值的片元（[`shaders/shape.frag:59-65`](shaders/shape.frag#L59-L65)），边界是硬的 | 需要设置阈值，硬边在旋转时闪烁 |
| alpha blend | 保留 alpha 交给固定功能混合（[`src/renderer.cpp:321-329`](src/renderer.cpp#L321-L329)） | 不写深度，必须由远到近排序 |
| alpha to coverage | 打开管线的 alpha to coverage 状态，按 alpha 对已通过覆盖与深度测试的采样点做概率判定（[`src/renderer.cpp:303-304`](src/renderer.cpp#L303-L304)） | 需要多重采样 |

三条几何管线由同一个函数按种类分支创建（[`src/renderer.cpp:277-365`](src/renderer.cpp#L277-L365)），
共用一份顶点与片元着色器（[`src/renderer.cpp:367-390`](src/renderer.cpp#L367-L390)），差别只在深度状态、
混合状态与 alpha to coverage 开关三项。

片元调用次数由片元着色器里的原子累加统计（[`shaders/shape.frag:14-16`](shaders/shape.frag#L14-L16)、
[`shaders/shape.frag:54`](shaders/shape.frag#L54)），累加目标是一块主机可见的存储缓冲，帧末在等过栅栏
之后回读并清零（[`src/renderer.cpp:683-688`](src/renderer.cpp#L683-L688)）。

## 渲染流程

```mermaid
graph LR
    A[植被卡片实例] --> B[几何通道<br/>多重采样颜色与深度]
    B --> C[硬件解析]
    C --> D[色调映射与输出编码]
    D --> E[交换链图像]
```

几何通道的渲染通道挂两个附件，多重采样的颜色附件与多重采样的深度附件
（[`src/renderer.cpp:50-105`](src/renderer.cpp#L50-L105)），附件尺寸与采样数在交换链目标里创建
（[`src/renderer.cpp:149-189`](src/renderer.cpp#L149-L189)）。解析把多重采样颜色合成到单采样贴图，
采样数为一的时候改用图像拷贝，因为单采样没有可解析的采样点
（[`src/renderer.cpp:821-845`](src/renderer.cpp#L821-L845)）。最后一个全屏三角形做色调映射与输出编码，
写入交换链图像（[`src/renderer.cpp:856-871`](src/renderer.cpp#L856-L871)）。

三种处理方式共用同一个渲染通道，每帧只切换绑定的管线（[`src/renderer.cpp:763-784`](src/renderer.cpp#L763-L784)）。
alpha test 与 alpha to coverage 写深度、按由近到远绘制，alpha blend 不测试也不写深度、按由远到近绘制。
形状缓冲里放两份同样的卡片，前半段由远到近，后半段由近到远（[`src/renderer.cpp:521-528`](src/renderer.cpp#L521-L528)），
绘制时按方式选择起始实例号跳进对应的一半（[`src/renderer.cpp:776-783`](src/renderer.cpp#L776-L783)）。

## 实现要点

### 三种方式的边界

掩码的每片叶片中间是实心，外侧留一段固定宽度的渐变，宽度是卡片边长的百分之零点七
（[`shaders/shape.frag:32`](shaders/shape.frag#L32)、[`shaders/shape.frag:42-44`](shaders/shape.frag#L42-L44)），
三种方式的差别全部落在这段渐变上。alpha test 把这段渐变按阈值劈成两半，低于阈值的一侧丢弃、另一侧
强制写成不透明（[`shaders/shape.frag:59-65`](shaders/shape.frag#L59-L65)）；alpha blend 与 alpha to
coverage 把这同一个 alpha 原样写进颜色附件的第四个通道（[`shaders/shape.frag:68`](shaders/shape.frag#L68)），
由固定功能阶段决定它的去向。在旋转角度固定为 0 时抓帧，取同一行的同一段像素，得到下面这串亮度值
（这一行的背景是灰阶 69，叶片实心处是 115）：

| 方式 | 该段像素的亮度 |
| --- | --- |
| alpha test 1x | 69 69 69 69 69 69 69 69 115 115 115 115 115 115 115 115 115 115 115 115 69 … |
| alpha blend 1x | 69 69 69 69 69 69 69 69 115 115 115 115 115 115 115 115 115 115 115 107 91 73 69 … |
| alpha to coverage 2x | 69 69 69 69 69 69 69 69 115 115 115 115 115 115 115 115 115 115 115 97 97 69 … |
| alpha to coverage 4x | 69 69 69 69 69 69 69 97 115 115 115 115 115 115 115 115 115 115 107 97 69 … |
| alpha to coverage 8x | 69 69 69 69 69 69 69 78 97 115 115 115 115 115 115 115 115 115 115 107 92 69 … |

alpha test 从 69 一步跳到 115，边界上没有中间值。alpha blend 从 115 经 107、91、73 落到 69，过渡最平滑。
alpha to coverage 只有采样点密度决定的那几级：2x 只多出一个 97，4x 出现 107 与 97 两级，8x 展开成
78、97、107、92 四级，向 blend 的连续过渡靠拢。

同一角度下三种方式的逐像素差别：

| 对比 | 差异像素占比 | 平均绝对差 | 最大差值 |
| --- | --- | --- | --- |
| test 1x 与 blend 1x | 3.117% | 0.695 | 49 |
| test 1x 与 coverage 4x | 3.675% | 0.723 | 62 |
| blend 1x 与 coverage 4x | 1.057% | 0.299 | 45 |

差别集中在叶片的边界上，卡片内部两种方式都落到同一个实心值。

### 旋转时的闪烁

旋转角度由每块卡片的相位与场景的累计角度相加得到，四个角点绕卡片中心转过这个角度
（[`shaders/shape.vert:35-39`](shaders/shape.vert#L35-L39)）。把旋转角度从 0 推到 0.04 弧度，比较两帧
之间每个像素的变化幅度：

| 方式 | 变化像素占比 | 其中幅度超过 40 的占比 |
| --- | --- | --- |
| alpha test 1x | 11.300% | 11.241% |
| alpha blend 1x | 14.220% | 9.421% |
| alpha to coverage 4x | 14.700% | 8.877% |

alpha test 的变化像素几乎全部是大跳变：边界像素在背景与实心之间来回翻转。blend 与 coverage 有更多
像素参与变化，但其中大跳变的比例低得多，边界是在若干灰阶之间移动。

### 设备时间与工作量

锁频 2880/15001 之后各测一段，结果写进 CSV：

| 方式 | 采样数 | 片元调用次数 | 绘制命令条数 | 设备时间 |
| --- | --- | --- | --- | --- |
| test | 1 | 934287 | 2 | 1.410 ± 0.176 ms |
| blend | 1 | 934287 | 2 | 1.437 ± 0.208 ms |
| coverage | 2 | 941042 | 2 | 1.432 ± 0.188 ms |
| coverage | 4 | 944775 | 2 | 1.449 ± 0.200 ms |
| coverage | 8 | 947297 | 2 | 1.491 ± 0.225 ms |
| test | 4 | 944775 | 2 | 1.454 ± 0.211 ms |

三种方式的绘制命令条数相同：几何用一次实例化绘制提交（[`src/renderer.cpp:783`](src/renderer.cpp#L783)），
之后是解析与全屏色调映射，共两条（[`src/renderer.cpp:870`](src/renderer.cpp#L870)）。设备时间在 1.41 到
1.49 毫秒之间，标准差与差值同量级，按本仓库的约定视为噪声，不构成结论。片元调用次数随采样数只上升约
百分之一点四，来自边界像素上被部分覆盖的图元被再次计入。

采样数是唯一需要重建资源的选项：它同时决定渲染通道、附件与全部管线，改动时先等设备空闲，再把管线、
渲染通道与交换链目标整套销毁重建（[`src/renderer.cpp:644-671`](src/renderer.cpp#L644-L671)）。

## 界面控件

| 控件 | 作用 |
| --- | --- |
| 方式 | alpha test、alpha blend 或 alpha to coverage |
| alpha 阈值 | alpha test 的丢弃阈值，也是 blend 与 coverage 保留 alpha 的下限参考 |
| 采样数 | 1x、2x、4x、8x，超过设备上限时自动降档 |
| 旋转速度 | 植被每秒转过的弧度，设为零可以固定角度抓帧 |
| 背景亮度 | 清除色的亮度 |
| GPU 锁频 | 与间接绘制 case 共用同一个面板 |

选中 alpha to coverage 且采样数为一的时候，面板给出提示，本帧按 alpha test 处理
（[`src/renderer.cpp:763-774`](src/renderer.cpp#L763-L774)）。面板同时显示绘制命令条数与片元调用次数，
另附操作指南；耗时面板按本 case 的通道拆分逐项列出。

## 命令行参数

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--mode 名字` | 处理方式，取 `test`、`blend` 或 `coverage` | coverage |
| `--samples N` | 采样数，取 1、2、4 或 8 | 4 |
| `--threshold F` | alpha 阈值，0 到 1 | 0.5 |
| `--background F` | 背景亮度，0 到 1 | 0.06 |
| `--rotation-speed F` | 植被每秒转过的弧度 | 0.6 |
| `--angle F` | 启动时的累计旋转角度，单位弧度 | 0 |
| `--no-interface` / `--auto-exit S` / `--capture F` / `--report F` / `--control-port N` | 与其他 case 一致 | |

键盘操作：`Esc` 退出。

## 测试方法

固定旋转角度，先抓三种方式在同一角度下的画面：

```
build\meow_alpha_modes.exe --mode test --samples 1 --rotation-speed 0 --angle 0 --auto-exit 4 --no-interface --capture intermediate\am_test_1x.png
build\meow_alpha_modes.exe --mode blend --samples 1 --rotation-speed 0 --angle 0 --auto-exit 4 --no-interface --capture intermediate\am_blend_1x.png
build\meow_alpha_modes.exe --mode coverage --samples 2 --rotation-speed 0 --angle 0 --auto-exit 4 --no-interface --capture intermediate\am_cov_2x.png
build\meow_alpha_modes.exe --mode coverage --samples 4 --rotation-speed 0 --angle 0 --auto-exit 4 --no-interface --capture intermediate\am_cov_4x.png
build\meow_alpha_modes.exe --mode coverage --samples 8 --rotation-speed 0 --angle 0 --auto-exit 4 --no-interface --capture intermediate\am_cov_8x.png
```

再把角度推到 0.04 弧度，按方式各抓一帧，比较连续两帧的逐像素差值：

```
build\meow_alpha_modes.exe --mode test --samples 1 --rotation-speed 0 --angle 0.04 --auto-exit 4 --no-interface --capture intermediate\am_flip_test.png
```

测量设备时间：启动一个进程锁频并打开控制服务，按段发送命令，程序把每段的结果追加到 CSV：

```
build\meow_alpha_modes.exe --no-interface --control-port 21000 --core-clock 2880 --memory-clock 15001 --report intermediate\am_measure.csv
```

连上 21000 端口后，`mode`/`samples`/`threshold`/`background`/`rotation-speed`/`angle` 改配置，`begin` 与
`end` 圈定一段测量，`quit` 退出。

## 源码结构

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、表面与交换链、主循环 |
| `src/renderer.cpp` | 几何、解析与色调映射通道，三条几何管线，形状缓冲的排序与上传 |
| `src/case_ui.cpp` | 本 case 的控制面板与全部界面文本 |
| `src/timing_items.cpp` | 本 case 的计时项定义 |
| `shaders/shape.vert` | 卡片展开、绕中心旋转与逻辑深度到裁剪空间 z 的映射 |
| `shaders/shape.frag` | 程序生成的草丛掩码、三种处理方式的分支与片元计数 |
| `shaders/fullscreen.vert` `shaders/tonemap.frag` | 全屏三角形与色调映射 |

## 安卓端差异

- 实例与设备申请 Vulkan 1.1；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 采样数上限由设备的 `framebufferColorSampleCounts` 与 `framebufferDepthSampleCounts` 共同决定，超出时
  自动降到相邻可选档位。
- 分块架构下 alpha to coverage 与片上存储的配合与桌面不同，带宽差别需要在真机上测量。
- 设备时间只在设备支持时间戳时测量。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 帧率上限 60 FPS，避免无界空转发热。

### 安卓测试方法

```
adb -s <serial> install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s <serial> logcat -s MeowVulkanDemo
```

应用内置回环 TCP 控制服务（端口 21000），安卓端经 `adb forward tcp:21000 tcp:21000` 映射到本机。
