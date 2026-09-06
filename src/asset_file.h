#pragma once

#include <cstddef>
#include <vector>

#ifdef __ANDROID__
struct AAssetManager;
// 安卓端在入口里把 AAssetManager 传进来，之后按名字读 APK 里 assets 目录下的文件
void setAndroidAssetManager(AAssetManager* assetManager);
#endif

// 读一个随包资源，name 是仓库内的相对路径，例如 assets/backpack/backpack.obj、
// shaders/gbuffer.vert.spv。安卓从 APK 的 assets 目录读，桌面从构建目录读
std::vector<unsigned char> readAssetBytes(const char* name);
