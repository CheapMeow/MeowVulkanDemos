# shader_throughput：着色吞吐与采样优化的微基准

构建方式见仓库根目录的 README。

## 简介

一个全屏的计算着色器，每个线程负责一个像素，线程组固定为八乘八
（[`shaders/throughput_common.glsl:4`](shaders/throughput_common.glsl#L4)），按界面参数在几组内核之间切换，
结果写进一块存储缓冲（[`shaders/throughput_common.glsl:14-16`](shaders/throughput_common.glsl#L14-L16)），
每个线程在自己的像素位置写一个浮点数（[`shaders/throughput_common.glsl:89`](shaders/throughput_common.glsl#L89)），
再由一个显示通道把它按灰度画到屏幕上（[`shaders/display.frag:19-22`](shaders/display.frag#L19-L22)），
整条链路保证计算不会被编译器优化掉。

| 内核 | 内容 |
| --- | --- |
| 算力密集 | 一串有数据依赖的正弦、余弦与小数取余，迭代次数可调（[`shaders/throughput_common.glsl:31-39`](shaders/throughput_common.glsl#L31-L39)） |
| 采样密集 | 在同一张纹理上取样，采样次数与采样局部性可调（[`shaders/throughput_common.glsl:41-58`](shaders/throughput_common.glsl#L41-L58)） |
| 分支 | 两条各自算力密集的分支，切换按像素发散还是按线程组一致（[`shaders/throughput_common.glsl:79-86`](shaders/throughput_common.glsl#L79-L86)） |

三个内核共用同一个入口，按 `kernelParams.x` 里的内核编号分派
（[`shaders/throughput_common.glsl:73-87`](shaders/throughput_common.glsl#L73-L87)）。采样用的纹理在启动时
按棋盘与噪声叠加生成，带足高频细节（[`src/renderer.cpp:30-45`](src/renderer.cpp#L30-L45)），采样器用线性
过滤与重复寻址（[`src/renderer.cpp:312-320`](src/renderer.cpp#L312-L320)）。

## 渲染流程

```mermaid
graph LR
    A[计算内核] --> B[存储缓冲]
    B --> C[显示通道]
    C --> D[交换链图像]
```

计算内核按每八乘八一个线程组派发，每个线程负责一个像素：派发尺寸把结果缓冲的宽高各加七之后除以八
（[`src/renderer.cpp:469-471`](src/renderer.cpp#L469-L471)），越界的线程在入口处直接返回
（[`shaders/throughput_common.glsl:63-66`](shaders/throughput_common.glsl#L63-L66)）。显示通道是一个全屏
三角形（[`shaders/fullscreen.vert:6-11`](shaders/fullscreen.vert#L6-L11)），把结果缓冲里的值按灰度取出来
（[`shaders/display.frag:17-22`](shaders/display.frag#L17-L22)）。两个通道之间隔一条内存屏障，把计算着色器
的写转换成片元着色器的读（[`src/renderer.cpp:476-481`](src/renderer.cpp#L476-L481)）。

## 实现要点

### 四组对比

锁频 2880/15001 之后按段测量，屏幕分辨率 1600 乘 900，每帧两万两千六百个线程组
（[`src/renderer.cpp:469-472`](src/renderer.cpp#L469-L472)）。设备时间取命令缓冲首尾两次时间戳查询的差值
（[`src/renderer.cpp:453-456`](src/renderer.cpp#L453-L456)、
[`src/renderer.cpp:555-557`](src/renderer.cpp#L555-L557)）：

| 内核 | 迭代次数 | 采样次数 | 分支发散 | 采样局部性 | 设备时间 |
| --- | --- | --- | --- | --- | --- |
| 算力密集 | 8 | | | | 0.021 ± 0.008 ms |
| 算力密集 | 32 | | | | 0.049 ± 0.012 ms |
| 算力密集 | 128 | | | | 0.160 ± 0.020 ms |
| 算力密集，中精度 | 32 | | | | 0.050 ± 0.013 ms |
| 分支 | 32 | | 否 | | 0.051 ± 0.013 ms |
| 分支 | 32 | | 是 | | 0.089 ± 0.015 ms |
| 采样密集 | | 4 | | 好 | 0.022 ± 0.010 ms |
| 采样密集 | | 16 | | 好 | 0.035 ± 0.012 ms |
| 采样密集 | | 16 | | 差 | 0.050 ± 0.012 ms |

算力密集内核的设备时间随迭代次数线性上升，从八次到一百二十八次是 0.021、0.049、0.160 毫秒，每帧的
固定开销约为 0.010 毫秒，斜率约为每次迭代 1.1 微秒，迭代次数决定内层循环绕几圈，每一圈里先做一次正弦
与余弦、再取一次小数（[`shaders/throughput_common.glsl:34-37`](shaders/throughput_common.glsl#L34-L37)）。
采样密集内核从四次到十六次是 0.022、0.035 毫秒，斜率约为每次采样 1.1 微秒，与一次迭代的算术成本同量级，
采样次数决定纹理取样循环的圈数（[`shaders/throughput_common.glsl:44-56`](shaders/throughput_common.glsl#L44-L56)）。

### 分支发散

同一个分支内核，把发散开关切换之后是 0.051 与 0.089 毫秒，比值 1.75。发散时同一个线程组里相邻线程
走不同的分支，两条分支都要执行再按掩码过滤；不发散时整组一起走同一条，只执行一条分支。两条分支的
迭代次数差一（[`shaders/throughput_common.glsl:86`](shaders/throughput_common.glsl#L86)），所以比值落在
2 以下。

发散开关决定取哪个判定式：打开时按像素坐标的奇偶取
（[`shaders/throughput_common.glsl:81-83`](shaders/throughput_common.glsl#L81-L83)），关闭时按线程组编号
的奇偶取（[`shaders/throughput_common.glsl:84`](shaders/throughput_common.glsl#L84)）。

### 采样局部性

同样是十六次采样，规整坐标是 0.035 毫秒，把坐标换成纹理上的随机位置之后是 0.050 毫秒，慢了百分之四十三。
采样次数相同、总字节数相同，差别只在相邻线程访问的纹素距离，命中的缓存层级不同。规整坐标是在像素
坐标上叠一个随采样序号增长的固定偏移，相邻线程落在邻近纹素上
（[`shaders/throughput_common.glsl:51-54`](shaders/throughput_common.glsl#L51-L54)）；随机坐标先用哈希把
像素坐标打散成 [0,1] 区间上的一点，相邻线程之间不再有位置关系
（[`shaders/throughput_common.glsl:23-28`](shaders/throughput_common.glsl#L23-L28)、
[`shaders/throughput_common.glsl:47-50`](shaders/throughput_common.glsl#L47-L50)）。

### 精度限定符

中精度与高精度都是 0.050 毫秒，桌面显卡把两者都编译成 32 位浮点。这一档的真正差别要在支持 16 位浮点
的安卓设备上才能量到，受限类型的判断需要厂商工具配合。两个版本各自声明默认精度之后再包含同一份内核
主体（[`shaders/throughput.comp:4`](shaders/throughput.comp#L4)、
[`shaders/throughput_half.comp:6`](shaders/throughput_half.comp#L6)），每帧按界面选择绑定对应的计算管线
（[`src/renderer.cpp:465-467`](src/renderer.cpp#L465-L467)）。

## 界面控件

| 控件 | 作用 |
| --- | --- |
| 类型 | 算力密集、采样密集或分支 |
| 默认精度 | 高精度或中精度 |
| 迭代次数 | 1 到 256 |
| 采样次数 | 1 到 64 |
| 分支发散 | 分支内核按像素发散还是按线程组一致 |
| 采样局部性差 | 采样密集内核用规整坐标还是随机坐标 |
| GPU 锁频 | 与间接绘制 case 共用同一个面板 |

面板同时显示线程组数量、绘制命令条数与两类内核的说明，另附操作指南；耗时面板按本 case 的阶段拆分
逐项列出。

## 命令行参数

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--kernel 名字` | 内核类型，取 `alu`、`sample` 或 `branch` | alu |
| `--precision 名字` | 默认精度，取 `high` 或 `medium` | high |
| `--iterations N` | 迭代次数，1 到 256 | 32 |
| `--samples N` | 采样次数，1 到 64 | 8 |
| `--divergent` | 分支内核按像素发散 | 关闭 |
| `--random-locality` | 采样密集内核用随机坐标 | 关闭 |
| `--no-interface` / `--auto-exit S` / `--capture F` / `--report F` / `--control-port N` | 与其他 case 一致 | |

键盘操作：`Esc` 退出。

## 测试方法

```
build\meow_shader_throughput.exe --kernel alu --iterations 32 --auto-exit 3 --no-interface --capture intermediate\st_alu.png
```

测量设备时间：启动一个进程锁频并打开控制服务，按段发送命令，程序把每段的结果追加到 CSV：

```
build\meow_shader_throughput.exe --no-interface --control-port 21000 --core-clock 2880 --memory-clock 15001 --report intermediate\st_measure.csv
```

连上 21000 端口后，`kernel`/`precision`/`iterations`/`samples`/`divergent`/`random-locality` 改配置，
`begin` 与 `end` 圈定一段测量，`quit` 退出。

## 源码结构

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、表面与交换链、主循环 |
| `src/renderer.cpp` | 计算管线的派发、结果缓冲与显示通道 |
| `src/case_ui.cpp` | 本 case 的控制面板与全部界面文本 |
| `src/timing_items.cpp` | 本 case 的计时项定义 |
| `shaders/throughput_common.glsl` | 三组内核的主体 |
| `shaders/throughput.comp` `shaders/throughput_half.comp` | 高精度与中精度两个版本，只有默认精度不同 |
| `shaders/fullscreen.vert` `shaders/display.frag` | 全屏三角形与结果缓冲的显示 |

## 安卓端差异

- 实例与设备申请 Vulkan 1.1；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 受限类型的判断（算力受限还是访存受限）需要安卓真机与 Snapdragon Profiler 的计数器互相印证。
- 中精度在支持 16 位浮点的设备上才会真正降精度，吞吐通常是高精度的两倍。
- 压缩纹理需要离线编码工具，本 case 没有包含这一档对照。
- 设备时间只在设备支持时间戳时测量。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 帧率上限 60 FPS，避免无界空转发热。

### 安卓测试方法

```
adb -s <serial> install -r android\app\build\outputs\apk\debug\app-debug.apk
adb -s <serial> logcat -s MeowVulkanDemo
```

应用内置回环 TCP 控制服务（端口 21000），安卓端经 `adb forward tcp:21000 tcp:21000` 映射到本机。
