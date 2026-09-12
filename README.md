# Meow Vulkan Demos

一组 Vulkan 示例的集合。每个示例是一个独立可构建的 case，同一份源码同时支持 Windows 与安卓，每个 case 都有自己的 README 说明其原理与测试方法。

## case 列表

| case | 内容 |
| --- | --- |
| [`cases/indirect_draw`](cases/indirect_draw/README.md) | 延迟渲染场景，对比逐实例、实例化与 indirect 三条几何提交路径 |
| [`cases/shadow`](cases/shadow/README.md) | 方向光阴影贴图，对比无阴影、硬阴影与 PCF 软阴影 |

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `common/` | 各 case 共用的代码：Vulkan 上下文与资源、资源读取、模型解析、场景与相机、计时统计、界面公共面板、TCP 控制服务、抓帧、锁频 |
| `cases/<名字>/` | 一个 case 的全部内容：入口、渲染器、控制面板、着色器、README |
| `android/` | 两个平台共用的 Gradle 工程，按 case 名字打包 |
| `scripts/` | 构建、资源下载与测量脚本 |
| `external/` | 第三方库，以 git submodule 引入 |
| `assets/` | 下载来的模型与贴图，不入库 |

## 依赖

第三方库以 git submodule 形式引入：

| 库 | 用途 |
| --- | --- |
| `external/glfw` | 窗口与输入（仅桌面构建使用） |
| `external/glm` | 矩阵与向量运算 |
| `external/stb` | 读取 jpg/png 纹理、写出抓取的画面 |
| `external/imgui` | 参数控制面板与耗时显示 |
| `external/implot` | 耗时曲线绘制 |

桌面的 Vulkan 头文件、`vulkan-1.lib` 与 `glslc.exe` 来自本机安装的 Vulkan SDK，路径由环境变量 `VULKAN_SDK_DIR` 提供，仓库内不写任何本机路径。安卓构建不需要 Vulkan SDK 的头文件与库：它们由 NDK 提供，只有 `glslc.exe` 需要在宿主机上编译着色器，同样经 `VULKAN_SDK_DIR` 或 PATH 找到；SPIR-V 的目标版本按平台区分（桌面 1.2，安卓 1.1）。

## 准备

```
git submodule update --init --recursive
scripts\fetch_assets.bat
```

`scripts\fetch_assets.bat` 下载 obj 模型与配套的 PBR 纹理（albedo、法线、金属度、粗糙度、环境光遮蔽）到 `assets/backpack`，各 case 共用。

## 桌面构建

```
scripts\build.bat [case]
```

case 省略时构建 `indirect_draw`。脚本调用 Visual Studio 自带的 CMake 与 Ninja 完成配置与编译，并用 `glslc` 把该 case `shaders` 目录下的 GLSL 编译成 SPIR-V 到 `build/shaders`。产物是 `build\meow_<case>.exe`。

依赖路径全部从环境变量读取，脚本里不含任何本机路径：`VS_DIR` 指向 Visual Studio 安装目录，`VULKAN_SDK_DIR` 指向 Vulkan SDK 目录。全局环境不满足时，在 `scripts\` 下放一个不入库的 `local_env.bat`，构建脚本检测到存在就先执行它，例如：

```
set "VS_DIR=D:\path\to\Visual Studio\2019\Community"
set "VULKAN_SDK_DIR=D:\path\to\VulkanSDK"
```

## 安卓构建

```
scripts\build_android.bat [case]
```

case 省略时构建 `indirect_draw`。产物是 `android\app\build\outputs\apk\debug\app-debug.apk`，一次只打包一个 case。

需要的环境变量同样在 `scripts\local_env.bat` 里给出：`JAVA_HOME` 指向 JDK 17，`ANDROID_HOME` 指向 Android SDK，桌面构建还用到 `VS_DIR` 与 `VULKAN_SDK_DIR`：

```
set "JAVA_HOME=D:\path\to\jdk17"
set "ANDROID_HOME=D:\path\to\android-sdk"
set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
set "VS_DIR=D:\path\to\Visual Studio\2019\Community"
set "VULKAN_SDK_DIR=D:\path\to\VulkanSDK"
```

`scripts\build.bat` 与 `scripts\build_android.bat` 检测到 `local_env.bat` 存在就先执行它，再校验相关变量是否指向有效安装。需要的 SDK 组件是 platform 35、build-tools 34、NDK 27.0.12077973 与 CMake 3.22.1，全部由 Gradle 按 `android\app\build.gradle` 里的声明使用，缺失时用 `sdkmanager` 安装。

工程结构：

- `android\` 是 Gradle 工程，只有一个 `:app` 模块。模块内的 `CMakeLists.txt` 把仓库根目录的 `CMakeLists.txt` 作为子目录加进来，NDK 工具链由 AGP 提供，源码与桌面端共用一份。
- 入口是各 case 的 `src\android_main.cpp`，走系统自带的 NativeActivity 与 NDK 的 `native_app_glue`，不写 Java 代码，也不依赖 AndroidX 库。
- 模型、贴图、字体、着色器全部作为 assets 打进 APK：Gradle 在打包前把仓库 `assets\backpack` 复制到 `android\app\src\main\assets\backpack`，把系统字体 `msyh.ttc` 复制到 `assets\fonts`；CMake 把编译出的 `.spv` 直接写进 `assets\shaders`。AAssetManager 以 APK 的 assets 目录为根，代码里资源名不带 `assets/` 前缀。这些目录是构建产物，不入库。
- 桌面代码读文件用的是 `readAssetBytes`，安卓端实现换成 `AAssetManager`，模型、贴图、SPIR-V 都以字节流形式从内存加载，调用方不区分平台。

## 资源来源

模型与纹理来自 [LearnOpenGL](https://learnopengl.com/data/models/backpack.zip)，原始模型作者 Berk Gedik。压缩包内的 `specular.jpg` 实际是金属度贴图，`diffuse.jpg` 实际是 albedo 贴图，各 case 按 PBR 语义使用它们。
