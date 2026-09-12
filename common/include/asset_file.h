#pragma once

#include <cstddef>
#include <vector>

#ifdef __ANDROID__
struct AAssetManager;
// 安卓端在入口里把 AAssetManager 传进来，之后按名字读 APK 里 assets 目录下的文件
void setAndroidAssetManager(AAssetManager* assetManager);
#endif

// 读一个随包资源，name 是相对根目录的路径，例如 backpack/backpack.obj、
// shaders/gbuffer.vert.spv。安卓的 AAssetManager 以 APK 的 assets 目录为根，
// 因此这里的名字一律不带 assets/ 前缀
std::vector<unsigned char> readAssetBytes(const char* name);
