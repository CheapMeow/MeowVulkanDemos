# Vulkan Indirect Draw 对比示例

Windows 与安卓平台上的 Vulkan 延迟渲染示例，用同一份场景、同一份着色器，对比三条几何提交路径：

- 逐实例路径：主机遍历全部实例做视锥剔除，为每个可见实例记录一条 `vkCmdDrawIndexed`
- 实例化路径：主机做同样的剔除，全部可见实例合并成一条 `vkCmdDrawIndexed`，实例数量为可见数量
- indirect 路径：计算着色器在显存中完成视锥剔除并填写绘制命令，主机只记录一条 `vkCmdDrawIndexedIndirect`

三条路径共用同一个顶点着色器与片元着色器，都通过可见列表间接寻址实例数据，因此输出画面逐像素相同，差异只体现在提交方式上。前两条路径的剔除结果写进同一个可见列表缓冲，区别只在于把可见列表下标交给 `firstInstance` 还是交给 `gl_InstanceIndex`。

## 依赖

第三方库以 git submodule 形式引入：

| 库 | 用途 |
| --- | --- |
| `external/glfw` | 窗口与输入（仅桌面构建使用） |
| `external/glm` | 矩阵与向量运算 |
| `external/stb` | 读取 jpg/png 纹理、写出抓取的画面 |
| `external/imgui` | 参数控制面板与耗时显示 |
| `external/implot` | 耗时曲线绘制 |

桌面的 Vulkan 头文件、`vulkan-1.lib` 与 `glslc.exe` 来自本机安装的 Vulkan SDK，路径由 CMake 变量 `VULKAN_SDK_DIR` 指定，默认 `D:/path/to/VulkanSDK`。安卓构建不需要 Vulkan SDK：头文件与 `libvulkan.so` 由 NDK 提供，`glslc.exe` 仍然在宿主机上编译着色器，只是 SPIR-V 的目标版本按平台区分（桌面 1.2，安卓 1.1）。

## 准备与构建

```
git submodule update --init --recursive
scripts\fetch_assets.bat
scripts\build.bat
```

`scripts\fetch_assets.bat` 下载 obj 模型与配套的 PBR 纹理（albedo、法线、金属度、粗糙度、环境光遮蔽）到 `assets/backpack`。

`scripts\build.bat` 调用 Visual Studio 自带的 CMake 与 Ninja 完成配置与编译，并用 `glslc` 把 `shaders` 下的 GLSL 编译成 SPIR-V 到 `build/shaders`。Visual Studio 的安装路径从环境变量 `VS_DIR` 读取，脚本里不含本机路径；全局环境不满足时，在 `scripts\` 下放一个不入库的 `local_env.bat` 来覆盖（见下方安卓构建一节对这套机制的统一说明）。

## 安卓构建

```
scripts\build_android.bat
```

产物是 `android\app\build\outputs\apk\debug\app-debug.apk`。构建脚本里不含任何本机路径，依赖路径全部从环境变量读取：`JAVA_HOME` 指向 JDK 17，`ANDROID_HOME` 指向 Android SDK。当机器的全局环境变量不满足要求时，在 `scripts\` 下放一个不入库的 `local_env.bat` 来覆盖，例如：

```
set "JAVA_HOME=D:\path\to\jdk17"
set "ANDROID_HOME=D:\path\to\android-sdk"
set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
```

同一个文件也被桌面构建使用，可以把 Visual Studio 路径一并写进去：

```
set "VS_DIR=D:\path\to\Visual Studio\2019\Community"
```

`scripts\build.bat` 与 `scripts\build_android.bat` 检测到 `local_env.bat` 存在就先执行它，再校验相关变量是否指向有效安装。需要的 SDK 组件是 platform 35、build-tools 34、NDK 27.0.12077973 与 CMake 3.22.1，全部由 Gradle 按 `android\app\build.gradle` 里的声明使用，缺失时用 `sdkmanager` 安装。

工程结构：

- `android\` 是 Gradle 工程，只有一个 `:app` 模块。模块内的 `CMakeLists.txt` 把仓库根目录的 `CMakeLists.txt` 作为子目录加进来，NDK 工具链由 AGP 提供，源码与桌面端共用一份。
- 入口是 `src\android_main.cpp`，走系统自带的 NativeActivity 与 NDK 的 `native_app_glue`，不写 Java 代码，也不依赖 AndroidX 库。窗口句柄到达时用 `vkCreateAndroidSurfaceKHR` 建表面，旋转或切后台导致的表面失效由现有交换链重建路径处理。
- 模型、贴图、字体、着色器全部作为 assets 打进 APK：Gradle 在打包前把仓库 `assets\backpack` 复制到 `android\app\src\main\assets\backpack`，把系统字体 `msyh.ttc` 复制到 `assets\fonts`；CMake 把编译出的 `.spv` 直接写进 `assets\shaders`。AAssetManager 以 APK 的 assets 目录为根，代码里资源名不带 `assets/` 前缀。这些目录是构建产物，不入库。
- 桌面代码读文件用的是 `readAssetBytes`，安卓端实现换成 `AAssetManager`，模型、贴图、SPIR-V 都以字节流形式从内存加载，调用方不区分平台。

与桌面的差异：

- 实例与设备申请 Vulkan 1.1，着色器以 `--target-env=vulkan1.1` 编译，覆盖只支持 1.1 的设备；`minSdk` 24 是 Vulkan 1.0 的最低 API 等级。
- 设备时间不打 timestamp：移动 GPU 的时间戳查询经常不可用或精度很差，因此安卓上不创建查询池、不写时间戳，`设备时间` 恒为零，主机侧各项计时照常。
- 剔除调度、G-Buffer 通道、光照通道、界面绘制段落在命令缓冲上打了 `VK_EXT_debug_utils` 标记，RenderDoc 截帧时按这些名字分段显示耗时。扩展不可用时标记为空操作。
- GPU 锁频面板不显示：它依赖桌面的 `nvidia-smi`。
- 相机固定在初始化位置，没有键盘输入；触摸事件交给 ImGui 的安卓后端，面板上的滑块和按钮可以直接操作。
- 帧率上限 60 FPS，避免无界空转发热。

安装与查看日志：

```
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
adb logcat -s VulkanIndirectDrawDemo
```

用 RenderDoc 在真机上截帧不需要在本机启动 qrenderdoc：`scripts\renderdoc_capture.bat start` 通过 adb 完成整套准备——启动设备上的 RenderDoc 远程服务 `org.renderdoc.renderdoccmd.arm64`，把 `VK_LAYER_RENDERDOC_Capture` 挂到本应用的包名上，再以带层的方式启动应用；`scripts\renderdoc_capture.bat stop` 撤销这些全局设置。截帧文件由设备端 RenderDoc 写入 `/sdcard/Android/media/com.example.vulkanindirectdrawdemo/files/RenderDoc/`。手机需要先安装 RenderDoc 的 `org.renderdoc.renderdoccmd.arm64` 与本应用。脚本复刻标准 RenderDoc 安卓挂载流程，纯批处理实现，不依赖 python。

应用内置回环 TCP 控制服务（桌面与安卓一致，端口 21000），测试脚本连接后按行发命令：`path`/`instances`/`lights`/`far` 改配置，`begin` 与 `end` 圈定一段测量（`end` 返回一行与 CSV 同格式的数据），`capture` 触发 RenderDoc 截一帧，`quit` 退出。安卓端经 `adb forward tcp:21000 tcp:21000` 把设备上的控制端口映射到本机。`scripts\measure_android.bat` 用这套接口在真机上跑与 `measure_pc.bat` 相同的配置网格，把每段返回的行写进 `intermediate\measure_report_android.csv`。

## 运行

```
build\VulkanIndirectDrawDemo.exe
```

界面面板负责全部参数控制与信息显示：

| 控件 | 作用 |
| --- | --- |
| 绘制路径单选按钮 | 在逐实例、实例化、indirect 三条路径之间切换 |
| 实例数量 | 参与绘制的实例数量，取值范围到实例容量为止 |
| 光源数量 | 参与光照计算的点光源数量 |
| 远裁剪面 | 决定视锥深度，也就决定可见实例数量 |
| 移动速度 | 相机移动速度 |
| GPU 锁频 | 显示探测到的 NVIDIA 显卡，按"查询当前频率"按钮调用 `nvidia-smi` 读一次实时核心/显存频率，两个下拉框列出该显卡支持的全部频率档位供选择，锁频/解锁按钮调用 `nvidia-smi -lgc/-lmc`/`-rgc/-rmc` |

`nvidia-smi` 由程序同步启动并等待结束，一次调用会阻塞主线程几十到一百多毫秒，因此只有按下按钮才会调用它，没有任何定时轮询，帧时间曲线不会被这种与绘制无关的因素干扰。

面板同时显示实例总数、可见实例数量与绘制命令条数，另附操作指南。

耗时统计区域按计时项逐一列出：帧时间、主机剔除、命令缓冲记录过程按功能步骤拆分出的各项（命令缓冲起始、剔除计算调度、G-Buffer 通道、光照通道、界面绘制、抓帧拷贝、提交命令）、设备时间。逐实例绘制路径下 G-Buffer 通道耗时项包含整条逐实例绘制循环，循环内部不逐次计时。每一项都显示最近一百帧的平均值与标准差，下方配一张曲线，横轴是启动以来经过的时间，只画出最近十秒；纵轴上下限取同一批数据的最小值与最大值，再各向外留一成余量，与横轴取自同一批数据。按时间而不是按帧数取窗口，不同帧率下横轴跨度一致，纵轴也不会把早已滚出画面的旧尖峰一直算进来。曲线图高一百五十像素，并且不画坐标轴标题：坐标轴刻度文字与四周留白固定占掉近七十像素，图矮的时候留给曲线的只剩十几像素，纵轴范围取得再准也看不出起伏。

界面上的实例数量与光源数量都在启动时按容量分配好了缓冲，滑动过程中不重建任何 Vulkan 资源。实例按到网格中心的距离排序，减少数量时留下的是相机附近的一团，场景密度保持不变。

### GPU 锁频

锁频的全部行为都由本机 `nvidia-smi`（NVIDIA 驱动自带）完成，程序只负责拼命令行和展示结果：

| 步骤 | 调用 | 说明 |
| --- | --- | --- |
| 探测 | `--query-gpu=index,name`、`--query-supported-clocks=gr`、`--query-supported-clocks=mem` | 取第一块 NVIDIA 显卡的名字与它支持的核心/显存频率档位，档位降序填进两个下拉框。探测不到显卡或调用失败时，面板里如实显示原因，不影响渲染继续运行 |
| 锁频 | `-lgc 下限,上限`、`-lmc 下限,上限` | 按下拉框选中的档位把核心与显存频率都固定成这一个值（上下限填同一个数） |
| 解锁 | `-rgc`、`-rmc` | 交还给驱动自动调频。程序退出前如果处于锁频状态会自动解锁 |

`--core-clock` 与 `--memory-clock` 让锁频在启动阶段就完成，不必先开界面点按钮：程序在两个档位列表里各取与传入值最接近的一项并锁定，然后把实际锁到的值和请求值一起打印出来。这两个参数必须同时给出，只给一个直接报错退出。请求值不在档位列表里是允许的，取最接近的那一项即可，例如请求 2800 MHz 而最接近的档位是 2797 MHz 时锁的就是 2797 MHz。

选中的档位不等于显卡实际跑到的频率，这一点必须注意：

- 下拉框里的档位只是驱动报告"支持"的取值，锁频命令下发之后，显卡仍受功耗、温度、电压与驱动策略约束，实际频率可能低于档位，也可能因为负载不足自己降到更低的一档；
- 消费级显卡常常直接拒绝 `-lgc`/`-lmc`，或者需要以管理员身份运行本程序才生效，此时面板会显示锁频失败以及 `nvidia-smi` 返回的原因；
- 因此锁频是否真的生效，只能看实测频率，不能看档位。面板下方的核心频率与显存频率两张图就是为此准备的。

这两张图的数据来自后台线程，每 1 秒调用一次 `nvidia-smi --query-gpu=clocks.gr,clocks.mem`，采样结果按启动以来的秒数记进历史。放在单独的线程里是为了让等待 `nvidia-smi` 的时间不落在主循环上——同步调用一次要几十到一百多毫秒，放在主线程里就会在帧时间曲线上留下每秒一个的尖峰。曲线的画法与耗时曲线完全一致：只画最近十秒，纵轴取这段数据的最小值与最大值再各向外留一成余量，横轴同为启动以来的秒数，纵轴单位为 MHz。`--no-interface` 的测量模式下不启动这个线程，测量过程完全不受采样影响。

面板上的"查询当前频率"按钮是另一条独立路径：它在主线程上同步查一次并把结果显示成文字，代价是这一次调用会卡住主线程。不想看到尖峰就不要按它，看曲线即可。

命令行参数用于自动化测量，与界面控制同一套状态：

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--instances N` | 启动时的实例数量 | 200000 |
| `--lights N` | 启动时的点光源数量 | 64 |
| `--far F` | 启动时的远裁剪面距离 | 160 |
| `--instanced` | 启动时使用实例化路径 | 关闭 |
| `--indirect` | 启动时使用 indirect 路径 | 关闭 |
| `--no-interface` | 不显示界面面板 | 显示 |
| `--switch-every S` | 每 S 秒依次切换到下一条绘制路径 | 关闭 |
| `--sweep-instances S` | 每 S 秒在若干实例数量之间轮换 | 关闭 |
| `--auto-exit S` | 运行 S 秒后自动退出 | 关闭 |
| `--core-clock N` | 启动时锁定的核心频率，单位 MHz，取最接近的可选档位 | 不锁频 |
| `--memory-clock N` | 启动时锁定的显存频率，单位 MHz，取最接近的可选档位 | 不锁频 |
| `--capture 文件名` | 退出前把画面写成 PNG 并打印像素统计 | 关闭 |
| `--report 文件名` | 退出前把本次运行的平均耗时以 CSV 形式追加到该文件 | 关闭 |

`--report` 由程序自己打开文件写入，测量结果不经过控制台重定向。文件为空时先写一行表头，之后每次运行追加一行：

```
draw_path,instances,visible_instances,draw_commands,frame_ms_avg,frame_ms_stddev,cpu_cull_ms_avg,cpu_cull_ms_stddev,
cpu_record_begin_ms_avg,cpu_record_begin_ms_stddev,cpu_record_cull_dispatch_ms_avg,cpu_record_cull_dispatch_ms_stddev,
cpu_record_gbuffer_pass_ms_avg,cpu_record_gbuffer_pass_ms_stddev,cpu_record_lighting_pass_ms_avg,cpu_record_lighting_pass_ms_stddev,
cpu_record_ui_ms_avg,cpu_record_ui_ms_stddev,cpu_record_capture_ms_avg,cpu_record_capture_ms_stddev,
cpu_record_submit_ms_avg,cpu_record_submit_ms_stddev,gpu_ms_avg,gpu_ms_stddev
```

每个计时项占两列，平均值与标准差各一列。统计跳过启动后的一点五秒预热，之后的每一帧都计入这个从预热结束到程序退出的完整窗口。程序的控制台输出全部使用英文。

实例容量取一百万与 `--instances` 的较大值，界面滑块的上限就是这个容量。

键盘操作：

| 按键 | 作用 |
| --- | --- |
| `W` `A` `S` `D` | 前后左右移动 |
| `Q` `E` | 下降与上升 |
| 方向键 | 转动视角 |
| 左 Shift | 加速移动 |
| 空格 | 依次切换到下一条绘制路径 |
| Esc | 退出 |

一键对比：

```
scripts\compare.bat 200000 420 8
```

三个参数依次为实例数量、远裁剪面距离、每条路径的运行秒数。脚本关闭界面分别运行三条路径，把耗时写进 `intermediate\compare_report.csv`，把三张 PNG 的 SHA256 写进 `intermediate\compare_hashes.csv`。

一致性验证与耗时测量（PC 与安卓分开）：

```
scripts\verify_paths.bat
scripts\measure_pc.bat
scripts\measure_android.bat
```

`verify_paths.bat` 在一千、两万、二十万、一百万实例下分别运行三条路径，对比可见实例数量。`measure_pc.bat` 在 PC 上、`measure_android.bat` 在安卓真机上执行同一套配置网格（三组实例数/远裁剪面 × 三条路径）的耗时测量。桌面版启动一个带 TCP 控制服务的进程，把每组配置当作一个测量分段，分段结束时把该段的均值/标准差追加进 `intermediate\measure_report.csv`；安卓版把设备上的控制端口经 `adb forward` 映射到本机，从每段的 `end` 回复里收集同一格式的行，写进 `intermediate\measure_report_android.csv`。桌面测量关闭界面，安卓测量保留界面。

全部脚本的提示文字与生成的报告都使用英文，报告一律是 CSV 格式，可以直接导入表格软件。

## 实测数据

测试环境为 NVIDIA GeForce RTX 5080，核心频率锁定 2880 MHz、显存频率锁定 15001 MHz（`scripts\measure_pc.bat` 的默认设置），分辨率 1600x900，模型 67907 个三角形，64 个点光源，呈现模式为立即模式，测量时关闭界面面板。以下数据取自 `scripts\measure_pc.bat` 生成的 CSV 报告的平均值列。关闭界面面板、不抓取画面时，记录：界面绘制与记录：抓帧拷贝这两项恒为零，下表不再单独列出。

20 万实例，远裁剪面 160，可见 435 个实例：

| 路径 | 绘制命令 | 帧时间 | 主机剔除 | 记录：起始 | 记录：剔除调度 | 记录：G-Buffer 通道 | 记录：光照通道 | 记录：提交命令 | 设备时间 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 逐实例 | 435 | 2.637 ms | 0.484 ms | 0.024 ms | 0.000 ms | 0.450 ms | 0.015 ms | 0.057 ms | 1.505 ms |
| 实例化 | 1 | 2.231 ms | 0.470 ms | 0.025 ms | 0.000 ms | 0.060 ms | 0.014 ms | 0.058 ms | 1.507 ms |
| indirect | 1 | 1.783 ms | 0.000 ms | 0.024 ms | 0.034 ms | 0.049 ms | 0.014 ms | 0.062 ms | 1.503 ms |

20 万实例，远裁剪面 420，可见 2905 个实例：

| 路径 | 绘制命令 | 帧时间 | 主机剔除 | 记录：起始 | 记录：剔除调度 | 记录：G-Buffer 通道 | 记录：光照通道 | 记录：提交命令 | 设备时间 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 逐实例 | 2905 | 12.988 ms | 0.546 ms | 0.041 ms | 0.000 ms | 2.664 ms | 0.030 ms | 0.081 ms | 9.490 ms |
| 实例化 | 1 | 10.303 ms | 0.548 ms | 0.036 ms | 0.000 ms | 0.079 ms | 0.018 ms | 0.075 ms | 9.435 ms |
| indirect | 1 | 9.788 ms | 0.000 ms | 0.038 ms | 0.052 ms | 0.073 ms | 0.018 ms | 0.082 ms | 9.412 ms |

100 万实例，远裁剪面 160，可见 435 个实例：

| 路径 | 绘制命令 | 帧时间 | 主机剔除 | 记录：起始 | 记录：剔除调度 | 记录：G-Buffer 通道 | 记录：光照通道 | 记录：提交命令 | 设备时间 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 逐实例 | 435 | 4.419 ms | 2.243 ms | 0.031 ms | 0.000 ms | 0.453 ms | 0.018 ms | 0.067 ms | 1.501 ms |
| 实例化 | 1 | 4.103 ms | 2.286 ms | 0.035 ms | 0.000 ms | 0.076 ms | 0.017 ms | 0.072 ms | 1.501 ms |
| indirect | 1 | 1.774 ms | 0.000 ms | 0.022 ms | 0.031 ms | 0.044 ms | 0.013 ms | 0.056 ms | 1.517 ms |

先定噪声的量级，再谈差距。记录：命令缓冲起始这一项在三条路径上做的是完全相同的事情（重置命令缓冲、重置时间戳查询池），实测均值 0.024 / 0.025 / 0.024 毫秒，标准差也在 0.011~0.018 毫秒。也就是说，工作内容完全相同的两项，测出来也会差出百分之一毫秒。因此下面把百分之一毫秒量级、与标准差同量级的差距一律视为噪声，按"相同"处理。

**主机剔除：逐实例与实例化相同。** 两条路径调用的是同一个主机剔除函数，实测 0.484 / 0.470（20 万实例、可见 435）、0.546 / 0.548（20 万实例、可见 2905）、2.243 / 2.286（100 万实例、可见 435），按上面的尺度两者没有区别。这一项由实例总量决定，与可见数量几乎无关：实例从 20 万涨到 100 万，它从约 0.5 毫秒涨到约 2.2 毫秒；可见实例从 435 涨到 2905，它只从 0.484 走到 0.546。

**设备时间：三条路径相同。** 三组分别是 1.505 / 1.507 / 1.503、9.490 / 9.435 / 9.412、1.501 / 1.501 / 1.517 毫秒，路径之间的相对差距都在百分之一以内（最大的一处是 9.490 对 9.412，差 0.078 毫秒）。像素与顶点的工作量一致，帧时间的差别全部落在主机侧。

**逐实例 → 实例化：只有记录：G-Buffer 通道变了。** 这一项从 0.450 降到 0.060（可见 435）、从 2.664 降到 0.079（可见 2905）、从 0.453 降到 0.076（100 万实例）。变化的内容就是把"每个可见实例一条 vkCmdDrawIndexed"换成"一条带实例数量的 vkCmdDrawIndexed"，折算下来每条命令约 0.9 微秒（可见 435 省 0.39 毫秒，可见 2905 省 2.59 毫秒）。主机剔除、记录：起始、记录：光照通道、记录：提交命令、设备时间全部不变。

**实例化 → indirect：只有主机剔除变了。** 主机剔除从 0.470 / 0.548 / 2.286 毫秒归零，剔除本身交给计算着色器，代价是多出记录：剔除调度这一项（0.031~0.052 毫秒）。记录：G-Buffer 通道在两条路径上都只有一条命令，实测 0.049~0.079 毫秒，同属一个量级，与逐实例路径不在同一个数量级；设备时间也没有因为剔除移到 GPU 而上升。

**两条改进各自对应一个规模量：**

- 实例化省掉的是"每个可见实例下发一条命令"的开销，收益随可见实例数量线性增长：可见 435 的两组帧时间分别省 0.41 与 0.32 毫秒，可见 2905 的那组省 2.69 毫秒。
- indirect 在此基础上省掉的是"在主机上遍历全部实例"的开销，收益随实例总量线性增长，与可见数量基本无关：20 万实例的两组帧时间分别省 0.45 与 0.52 毫秒，100 万实例的那组从 4.103 降到 1.774 毫秒，省 2.33 毫秒。

把可见实例数量和实例总量分开看就能预测该选哪条路径：可见实例多、实例总量小，实例化已经拿走了绝大部分收益；实例总量大，收益几乎全部来自 indirect。100 万实例那组最典型——可见实例只有 435 个，实例化只省了 0.32 毫秒，indirect 却省了 2.33 毫秒。

三条路径抓取的 PNG 校验值相同，可见剔除结果与渲染结果完全一致。

在一千、两万、二十万、一百万实例下，三条路径给出的可见实例数量分别为 269、666、666、666，逐项相同。

运行时反复切换路径并同时改变实例数量，可见实例数量始终与另外两条路径吻合，验证层没有报告任何问题。

## 渲染流程

```mermaid
graph LR
    A[实例数据] --> B{绘制路径}
    B -->|逐实例| C[主机视锥剔除<br/>写入可见列表]
    B -->|实例化| C
    B -->|indirect| D[计算着色器视锥剔除<br/>原子累加实例数量]
    C --> E[几何通道<br/>写入 G-Buffer]
    D --> E
    E --> F[光照通道<br/>PBR 直接光照]
    F --> G[交换链图像]
```

前两条路径在这张图上走同一条分支，它们的差别只在几何通道里提交了多少条绘制命令。

G-Buffer 由三张颜色附件与一张深度附件组成：

| 附件 | 格式 | 内容 |
| --- | --- | --- |
| 0 | `R8G8B8A8_UNORM` | albedo 三通道与环境光遮蔽 |
| 1 | `R16G16B16A16_SFLOAT` | 世界空间法线与粗糙度 |
| 2 | `R16G16B16A16_SFLOAT` | 世界空间位置与金属度 |
| 深度 | `D32_SFLOAT` | 深度 |

法线贴图通过屏幕空间导数构造切线空间，因此 obj 解析器只需要提供位置、纹理坐标与法线。光照通道使用 Cook-Torrance BRDF：GGX 法线分布、Schlick-GGX 几何项与 Schlick 菲涅尔近似，配合 Reinhard 色调映射与伽马校正。界面在光照通道内、全屏三角形之后绘制。

界面文本使用系统自带的微软雅黑，字形范围由界面文本本身构建，启动时逐个码点确认字体里有对应字形，缺字直接终止。

## 源码结构

| 文件 | 内容 |
| --- | --- |
| `src/main.cpp` | 桌面入口：命令行解析、主循环与界面状态 |
| `src/android_main.cpp` | 安卓入口：NativeActivity 生命周期、ANativeWindow 表面与交换链、主循环 |
| `src/asset_file.cpp` | 随包资源读取：桌面读构建目录文件，安卓读 APK 的 assets |
| `src/vk_marker.cpp` | 命令缓冲调试标记的入口函数加载与 begin/end 调用 |
| `src/vk_context.cpp` | 实例、调试信息回调、物理设备、逻辑设备、窗口表面与交换链 |
| `src/vk_resources.cpp` | 缓冲与纹理的创建上传、多级渐远纹理生成、着色器模块加载 |
| `src/obj_loader.cpp` | obj 解析、顶点去重、模型居中与包围球计算 |
| `src/scene.cpp` | 实例网格生成与排序、跟随相机的光源平铺、相机控制、视锥平面提取、主机侧剔除 |
| `src/renderer.cpp` | 渲染通道、描述符、三条管线、三条绘制路径与时间戳查询（安卓上不查询） |
| `src/timing.cpp` | 计时项定义、滑动窗口统计、启动至今的历史曲线、测量报告的在线统计 |
| `src/user_interface.cpp` | imgui 与 implot 初始化、字体与字形校验、控制面板、耗时曲线绘制 |
| `src/gpu_clock_lock.cpp` | 桌面实现调用 `nvidia-smi` 探测显卡与锁频，安卓实现为整组"不可用" |
| `src/frame_capture.cpp` | 画面回读、像素统计与 PNG 写出 |
| `shaders/cull.comp` | 视锥剔除与绘制命令填写 |
| `shaders/gbuffer.vert` `shaders/gbuffer.frag` | 几何通道 |
| `shaders/fullscreen.vert` `shaders/lighting.frag` | 光照通道 |

## 资源来源

模型与纹理来自 [LearnOpenGL](https://learnopengl.com/data/models/backpack.zip)，原始模型作者 Berk Gedik。压缩包内的 `specular.jpg` 实际是金属度贴图，`diffuse.jpg` 实际是 albedo 贴图，本示例按 PBR 语义使用它们。
