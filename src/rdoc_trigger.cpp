#include "rdoc_trigger.h"

#include "renderdoc_app.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

void renderdocTriggerCapture()
{
#ifdef _WIN32
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr) {
        return;
    }
    const pRENDERDOC_GetAPI getApi =
        reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (getApi == nullptr) {
        return;
    }
    RENDERDOC_API_1_6_0* api = nullptr;
    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1 || api == nullptr) {
        return;
    }
    api->TriggerCapture();
#else
    // RTLD_NOLOAD：RenderDoc 层已经在进程里才去取，绝不主动加载
    void* module = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr) {
        return;
    }
    const pRENDERDOC_GetAPI getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
    if (getApi == nullptr) {
        return;
    }
    RENDERDOC_API_1_6_0* api = nullptr;
    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1 || api == nullptr) {
        return;
    }
    api->TriggerCapture();
#endif
}
