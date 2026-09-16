# bloom：泛光与亮部闪烁

构建方式见仓库根目录的 README。

## 简介

暗背景上有一个缓慢移动的高动态范围光源球（[`shaders/scene.frag:20-24`](shaders/scene.frag#L20-L24)），另加几条宽度不足一个像素的细亮条
（[`shaders/scene.frag:26-28`](shaders/scene.frag#L26-L28)）。整条泛光链全程在线性高动态范围空间里做，链上的每一张贴图都是十六位浮点格式
（[`src/renderer.cpp:13`](src/renderer.cpp#L13)、[`src/renderer.cpp:94-105`](src/renderer.cpp#L94-L105)）。

| 控件 | 取值 |
| --- | --- |
| 亮部取出 | 不做泛光、硬阈值、软阈值（[`src/renderer.h:14-19`](src/renderer.h#L14-L19)） |
| 降采样链 | 逐级高斯、一降一升、一次采样多层级（[`src/renderer.h:21-26`](src/renderer.h#L21-L26)） |
| 层级数 | 1 到 5（[`src/renderer.h:12`](src/renderer.h#L12)） |
| 阈值与泛光强度 | 阈值 0.5 到 6，强度 0 到 1.5 |
| 抗锯齿与色调映射的顺序 | 先抗锯齿、先色调映射（[`src/renderer.h:28-32`](src/renderer.h#L28-L32)） |

## 渲染流程

```mermaid
graph LR
    A[场景] --> B[场景通道<br/>高动态范围]
    B --> C[亮部取出并降到第一级]
    C --> D[逐级降采样]
    D --> E[逐级上采样叠加]
    D --> F[一次采样多层级]
    E --> G[合成、抗锯齿与色调映射]
    F --> G
    G --> H[交换链图像]
```

场景通道画一个全屏三角形，片元里按时间生成光源球与细亮条（[`src/renderer.cpp:566-588`](src/renderer.cpp#L566-L588)、
[`shaders/scene.frag:15-31`](shaders/scene.frag#L15-L31)）。亮部取出与第一级降采样共用一趟
（[`src/renderer.cpp:595-621`](src/renderer.cpp#L595-L621)）；逐级高斯与一降一升在循环里逐级下采
（[`src/renderer.cpp:628-651`](src/renderer.cpp#L628-L651)），再逐级放大叠加
（[`src/renderer.cpp:653-676`](src/renderer.cpp#L653-L676)）。一次采样多层级走另一条分支，五级拷完之后由合成通道按权重一次取出
（[`src/renderer.cpp:677-726`](src/renderer.cpp#L677-L726)）。最后全屏通道把场景与泛光合成，按选定的先后做抗锯齿与色调映射
（[`src/renderer.cpp:730-758`](src/renderer.cpp#L730-L758)、
[`shaders/present.frag:56-74`](shaders/present.frag#L56-L74)）。

## 实现要点

### 三种链实现与两种阈值

同一个时刻抓帧，与不做泛光的画面比较：

| 配置 | 平均绝对差 | 差值超过 8 的像素占比 | 最大差值 |
| --- | --- | --- | --- |
| 硬阈值，逐级高斯 | 0.032 | 0.000% | 6 |
| 软阈值，逐级高斯 | 0.032 | 0.000% | 6 |
| 硬阈值，一降一升 | 0.032 | 0.000% | 6 |
| 硬阈值，一次采样多层级 | 0.133 | 0.835% | 16 |
| 硬阈值，逐级高斯，先色调映射后抗锯齿 | 1.549 | 4.961% | 52 |

逐级高斯与一降一升的结果逐像素一致：逐级高斯在中心点与四条轴上各取若干点，权重按距离给出
（[`shaders/downsample.frag:30-41`](shaders/downsample.frag#L30-L41)），一降一升取中心加四个角
（[`shaders/downsample.frag:42-50`](shaders/downsample.frag#L42-L50)）；两者都是每一次只把上一级叠一层，
核的形状不同但叠加顺序相同，在这张只有一片柔和光晕的画面上看不出差别。叠加由固定功能混合完成
（[`src/renderer.cpp:306-315`](src/renderer.cpp#L306-L315)），渲染通道保留本级原有的内容
（[`src/renderer.cpp:42-54`](src/renderer.cpp#L42-L54)）。一次采样多层级把五级按权重一次合成
（[`shaders/combine.frag:18-27`](shaders/combine.frag#L18-L27)），粗层级的权重比逐级叠加高，光晕因此更大，
最大差值到十六。

硬阈值用阶跃直接截断亮度低于阈值的像素，软阈值在阈值附近留一段二次过渡
（[`shaders/downsample.frag:52-63`](shaders/downsample.frag#L52-L63)）。硬阈值与软阈值在这一时刻的差别
小于千分之一：光源的亮度远高于阈值，两者的分歧只落在过渡带那一条很窄的环上。

### 抗锯齿与色调映射的先后

同样的泛光，把抗锯齿从色调映射之前挪到之后（[`shaders/present.frag:64-72`](shaders/present.frag#L64-L72)），与不泛光的画面相差 1.549，
最大差值 52，变化的像素接近百分之五。抗锯齿沿边缘走向取四个邻域，按对比度决定混合比例（[`shaders/present.frag:31-54`](shaders/present.frag#L31-L54)
），色调映射是 Reinhard 加伽马编码（[`shaders/present.frag:17-21`](shaders/present.frag#L17-L21)
）。排在之前时平滑作用在高的动态范围上，光源的高光参与加权；排在之后时高光已经被压到显示范围内，再平滑得到的过渡带明显更窄。这一档是本 case 里差别最明显的一项。

### 光源移动时的帧间差

阈值取 1.0、强度取 1.0（[`shaders/downsample.frag:54`](shaders/downsample.frag#L54)、
[`shaders/present.frag:60`](shaders/present.frag#L60)），把时刻从 0.00 推到 0.03 秒再各抓一帧：

| 阈值 | 平均绝对差 | 差值超过 8 的像素占比 | 最大差值 |
| --- | --- | --- | --- |
| 硬阈值 | 0.282 | 1.421% | 15 |
| 软阈值 | 0.282 | 1.421% | 15 |

两个阈值在这一步的帧间差相同。光源的移动幅度在当前阈值与强度下不到一个像素（[`shaders/scene.frag:20-22`](shaders/scene.frag#L20-L22)），硬阈值截断出来的那一圈
还没有发生整块进出，闪烁要在这条分界更靠近光源的亮度时才会显现。

## 界面控件

| 控件 | 作用 |
| --- | --- |
| 亮部取出 | 不做泛光、硬阈值或软阈值 |
| 降采样链 | 三种实现 |
| 层级数 | 1 到 5，每加一级多两趟全屏拷贝 |
| 阈值与泛光强度 | 泛光的两个主要参数 |
| 抗锯齿与色调映射 | 先后顺序 |
| GPU 锁频 | 与间接绘制 case 共用同一个面板 |

面板同时显示绘制命令条数与渲染通道数，另附操作指南；耗时面板按本 case 的通道拆分逐项列出。

## 命令行参数

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--threshold 名字` | 亮部取出，取 `none`、`hard` 或 `soft` | hard |
| `--chain 名字` | 降采样链，取 `gaussian`、`kawase` 或 `multi` | gaussian |
| `--levels N` | 层级数，1 到 5 | 4 |
| `--threshold-value F` | 阈值 | 1.5 |
| `--intensity F` | 泛光强度 | 0.6 |
| `--order 名字` | 顺序，取 `aa_first` 或 `tonemap_first` | aa_first |
| `--time F` / `--freeze` | 动画时刻与冻结 | 0，关闭 |
| `--no-interface` / `--auto-exit S` / `--capture F` / `--report F` / `--control-port N` | 与其他 case 一致 | |

键盘操作：`Esc` 退出。

## 测试方法

```
build\meow_bloom.exe --threshold none --time 0 --freeze --auto-exit 3 --no-interface --capture intermediate\bl_none.png
build\meow_bloom.exe --threshold hard --chain gaussian --time 0 --freeze --auto-exit 3 --no-interface --capture intermediate\bl_hard.png
build\meow_bloom.exe --threshold soft --chain gaussian --time 0 --freeze --auto-exit 3 --no-interface --capture intermediate\bl_soft.png
build\meow_bloom.exe --threshold hard --chain multi --time 0 --freeze --auto-exit 3 --no-interface --capture intermediate\bl_multi.png
build\meow_bloom.exe --threshold hard --chain gaussian --order tonemap_first --time 0 --freeze --auto-exit 3 --no-interface --capture intermediate\bl_order.png
```

测量设备时间：启动一个进程锁频并打开控制服务，按段发送命令，程序把每段的结果追加到 CSV：

```
build\meow_bloom.exe --no-interface --control-port 21000 --core-clock 2880 --memory-clock 15001 --time 0 --freeze --report intermediate\bl_measure.csv
```

连上 21000 端口后，`threshold`/`chain`/`levels`/`threshold-value`/`intensity`/`order`/`time` 改配置，
`begin` 与 `end` 圈定一段测量，`quit` 退出。

## 源码结构

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、表面与交换链、主循环 |
| `src/renderer.cpp` | 场景通道、亮部取出与降采样链、上采样链、一次采样多层级与合成通道 |
| `src/case_ui.cpp` | 本 case 的控制面板与全部界面文本 |
| `src/timing_items.cpp` | 本 case 的计时项定义 |
| `shaders/scene.frag` | 程序生成的高动态范围场景 |
| `shaders/downsample.frag` | 亮部取出与逐级降采样，两套取样核 |
| `shaders/upsample.frag` `shaders/combine.frag` | 逐级叠加与一次采样多层级 |
| `shaders/present.frag` `shaders/fullscreen.vert` | 合成、抗锯齿两种顺序与色调映射 |

## 安卓端差异

- 实例与设备申请 Vulkan 1.1；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 分块架构上每一次切换渲染目标都是一次片上存储的加载与写出，层级数的代价比桌面更大。
- 设备时间只在设备支持时间戳时测量。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 帧率上限 60 FPS，避免无界空转发热。

### 安卓测试方法

```
adb -s <serial> install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s <serial> logcat -s MeowVulkanDemo
```

应用内置回环 TCP 控制服务（端口 21000），安卓端经 `adb forward tcp:21000 tcp:21000` 映射到本机。
