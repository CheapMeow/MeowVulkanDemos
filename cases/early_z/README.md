# early_z：early-Z、深度预通道与 discard

构建方式见仓库根目录的 README。

## 简介

一叠相互交叠的四边形沿视线方向一层层排开，其中一片是程序生成掩码的镂空植被。相机正对这一叠四边形，片元着色器里按界面上的"片元开销"做一段可以调长的循环，让着色开销大到足以看出 early-Z 省下了多少。

界面上的开关：

| 控件 | 作用 |
| --- | --- |
| 实例数量 | 参与绘制的四边形层数 |
| 片元开销 | 片元着色器里循环的次数，放大着色开销 |
| 绘制顺序 | 前到后、后到前、乱序三种提交顺序 |
| 深度预通道 | 主通道之前先只写一遍深度 |
| 插入永不成立的 discard | 在片元着色器里放一条永远不成立的 discard |
| 镂空走 alpha test | 植被片的镂空掩码是否参与 alpha test |

绘制顺序不改变实例数据，只改写一份顺序缓冲：顶点着色器按 `gl_InstanceIndex` 从顺序缓冲取出实例编号，再取实例数据。

## 渲染流程

```mermaid
graph LR
    A[四边形实例] --> B[深度预通道<br/>只写深度]
    A --> C[主通道<br/>着色与颜色写入]
    B --> C
    C --> D[交换链图像]
```

两个通道在同一个渲染通道内先后执行，深度附件在两者之间保留。有预通道时主通道用 `VK_COMPARE_OP_EQUAL` 判定并且不再写深度，没有预通道时用 `VK_COMPARE_OP_LESS` 并写入深度。

## 实现要点

### 顺序为什么影响耗时

early-Z 在片元着色之前完成深度测试，被遮挡的片元直接跳过着色。前到后的顺序让每一层四边形先把近处的深度写进去，随后更远的层整片被拒绝，实际着色的片元数从"层数乘以面积"降到接近"一层的面积"。后到前的顺序恰好相反，每一层都被判定为更近，全部走完整着色。

### discard 与 alpha test

片元着色器里出现 `discard`、写出深度或 alpha test 时，图形处理器在着色前拿不到确定的深度，很多实现会退回到着色之后再测深度，early-Z 随之失效。把 alpha test 挪进深度预通道、主通道改用相等判定，可以让第二次着色重新拿到 early-Z：预通道只写深度，代价很低，主通道再做完整着色。

这条结论与硬件实现强相关。本 case 的实测（见下）在本机的 NVIDIA 显卡上只看到绘制顺序与预通道的影响，没有看到 discard 带来的差异。

## 界面控件

| 控件 | 作用 |
| --- | --- |
| 实例数量 | 参与绘制的四边形层数，上限 256 |
| 移动速度 | 相机移动速度 |
| 片元开销 | 片元着色器里循环的次数，1 到 256 |
| 绘制顺序 | 前到后、后到前、乱序 |
| 深度预通道 | 开关预通道，改动后重建主通道管线 |
| 插入永不成立的 discard | 在片元着色器里放一条永假的 discard |
| 镂空走 alpha test | 打开后植被片按掩码丢弃片元 |
| GPU 锁频 | 与间接绘制 case 共用同一个面板 |

面板同时显示绘制命令条数与场景说明，另附操作指南；耗时面板按本 case 的通道拆分逐项列出。

## 命令行参数

命令行参数与界面控制同一套状态，用于自动化测试：

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--instances N` | 启动时的四边形层数 | 48 |
| `--order 名字` | 绘制顺序，取 `front`、`back` 或 `random` | front |
| `--prepass` | 启动时开启深度预通道 | 关闭 |
| `--discard` | 启动时插入永不成立的 discard | 关闭 |
| `--no-alpha-test` | 启动时关闭植被片的 alpha test | 开启 |
| `--fragment-cost N` | 片元着色器的循环次数，取值 1 到 256 | 64 |
| `--no-interface` | 不显示界面面板 | 显示 |
| `--auto-exit S` | 运行 S 秒后自动退出 | 关闭 |
| `--capture 文件名` | 退出前把画面写成 PNG 并打印像素统计 | 关闭 |
| `--report 文件名` | 退出前把本次运行的平均耗时以 CSV 形式追加到该文件 | 关闭 |
| `--control-port N` | 打开 TCP 控制服务并监听该端口 | 关闭 |
| `--core-clock N` | 启动时锁定的核心频率，单位 MHz，取最接近的可选档位 | 不锁频 |
| `--memory-clock N` | 启动时锁定的显存频率，单位 MHz，取最接近的可选档位 | 不锁频 |

键盘操作：`W` `A` `S` `D` 前后左右移动，`Q` `E` 下降与上升，方向键转动视角，左 Shift 加速，Esc 退出。

## 测试方法

`--fragment-cost 256 --instances 48` 下逐项测量设备时间：

```
build\meow_early_z.exe --auto-exit 5 --no-interface --fragment-cost 256 --report intermediate\early_z.csv --order front
build\meow_early_z.exe --auto-exit 5 --no-interface --fragment-cost 256 --report intermediate\early_z.csv --order back
```

本机 NVIDIA 显卡上的实测（设备时间均值）：

| 绘制顺序 | 深度预通道 | discard | 设备时间 |
| --- | --- | --- | --- |
| 前到后 | 关 | 关 | 0.094 ms |
| 后到前 | 关 | 关 | 0.308 ms |
| 乱序 | 关 | 关 | 0.151 ms |
| 前到后 | 关 | 开 | 0.094 ms |
| 前到后 | 开 | 关 | 0.035 ms |
| 后到前 | 开 | 关 | 0.039 ms |
| 前到后 | 开 | 开 | 0.035 ms |

前到后比后到前快约 3.3 倍，乱序落在两者之间，与"前到后最省着色开销"一致。加预通道之后耗时降到 0.035 ms，而且后到前也降到 0.039 ms，顺序的影响几乎消失，说明预通道把被遮挡片元的着色成本提前挡掉了。插入 discard 前后没有可测差别，开关 alpha test 同样没有差别，这与硬件实现有关：本机的实现即便着色器里存在 discard，仍然在着色之前完成深度测试。

镂空掩码的可见效果可以抓帧对比：

```
build\meow_early_z.exe --auto-exit 4 --no-interface --fragment-cost 16 --capture intermediate\early_z_alpha.png
build\meow_early_z.exe --auto-exit 4 --no-interface --fragment-cost 16 --capture intermediate\early_z_noalpha.png --no-alpha-test
```

两张图的差别是每像素平均 2.0，3.4% 的像素不同，差别集中在最前面那片植被的镂空边缘。

参数可以在同一次运行里改变：

```
adb -s <serial> forward tcp:21000 tcp:21000
```

桌面端用 `--control-port 21000` 启动后，连接并逐行发命令：`instances`/`order`/`prepass`/`discard`/`alpha-test`/`fragment-cost` 改配置，`begin` 与 `end` 圈定一段测量（`end` 返回一行与 CSV 同格式的数据），`quit` 退出。

随时间变化的参数曲线与测量报告格式与间接绘制 case 一致，报告里的前几列是绘制顺序、预通道开关、discard 开关、alpha test 开关、片元开销与绘制命令条数。

## 源码结构

本 case 自己的文件都在 `cases/early_z` 下：

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、ANativeWindow 表面与交换链、主循环 |
| `src/renderer.cpp` | 渲染通道、预通道与主通道两条管线、顺序缓冲、时间戳查询 |
| `src/scene_setup.cpp` | 面向相机的四边形网格 |
| `src/case_ui.cpp` | 本 case 的控制面板与全部界面文本 |
| `src/timing_items.cpp` | 本 case 的计时项定义 |
| `shaders/quad.vert` | 按顺序缓冲取实例、变换到裁剪空间 |
| `shaders/prepass.frag` | 深度预通道的片元着色，只保留 discard 逻辑 |
| `shaders/shade.frag` | 主通道的片元着色，含可调长度的循环 |
| `shaders/quad_common.glsl` | 实例数据、顺序缓冲与 discard 逻辑 |

## 安卓端差异

- 实例与设备申请 Vulkan 1.1，着色器以 `--target-env=vulkan1.1` 编译，覆盖只支持 1.1 的设备；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 设备时间只在设备支持时间戳时测量：驱动的 `timestampComputeAndGraphics` 能力与图形队列族的 `timestampValidBits` 都满足才创建查询池并记录时间戳，不支持的设备上"设备时间"恒为零，主机侧各项计时照常。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 相机固定在初始化位置，没有键盘输入；触摸事件交给 ImGui 的安卓后端，面板上的滑块和按钮可以直接操作。
- 帧率上限 60 FPS，避免无界空转发热。安卓上的分块架构对 early-Z 的处理与桌面不同，discard 与 alpha test 的影响需要在真机上重新测量。

### 安卓测试方法

adb 的目标设备由设备序列号指定，序列号用 `adb devices` 查询。只连接一台设备时命令里的 `-s <serial>` 可以省略。构建出 APK 后按下面方式安装并查看日志：

```
adb -s <serial> install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s <serial> logcat -s MeowVulkanDemo
```

应用内置回环 TCP 控制服务（桌面与安卓一致，端口 21000），安卓端经 `adb forward tcp:21000 tcp:21000` 把设备上的控制端口映射到本机，命令与桌面端相同。
