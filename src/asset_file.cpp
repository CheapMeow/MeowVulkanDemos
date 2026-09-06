#include "asset_file.h"

#include "vk_check.h"

#include <cstdio>
#include <string>

#ifdef __ANDROID__
#include <android/asset_manager.h>

static AAssetManager* gAssetManager = nullptr;

void setAndroidAssetManager(AAssetManager* assetManager)
{
    gAssetManager = assetManager;
}
#endif

std::vector<unsigned char> readAssetBytes(const char* name)
{
#ifdef __ANDROID__
    if (gAssetManager == nullptr) {
        FATAL("asset manager is not set before reading asset %s", name);
    }

    AAsset* asset = AAssetManager_open(gAssetManager, name, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        FATAL("cannot open asset %s", name);
    }

    const off_t length = AAsset_getLength(asset);
    std::vector<unsigned char> data(static_cast<size_t>(length));
    const int readBytes = AAsset_read(asset, data.data(), data.size());
    AAsset_close(asset);
    if (readBytes != static_cast<int>(length)) {
        FATAL("incomplete read of asset %s", name);
    }
    return data;
#else
    const std::string path = std::string(ASSET_ROOT_DIR) + "/" + name;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        FATAL("cannot open asset file %s", path.c_str());
    }

    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    std::vector<unsigned char> data(static_cast<size_t>(length));
    const size_t readBytes = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (readBytes != data.size()) {
        FATAL("incomplete read of asset file %s", path.c_str());
    }
    return data;
#endif
}
