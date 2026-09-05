#include "console.h"

#include <windows.h>

#include <cstdio>

// 进入程序时控制台的输出代码页，为 0 表示无需恢复
static UINT gPreviousOutputCodePage = 0;

void configureConsoleEncoding()
{
    // 源码与全部输出文本都是 UTF-8，控制台按自己的代码页解释字节，
    // 两者不一致时中文就会显示成乱码
    const UINT currentCodePage = GetConsoleOutputCP();

    // 没有附加控制台时返回 0，此时输出已被重定向到文件，写入的 UTF-8 字节无需处理
    if (currentCodePage == 0 || currentCodePage == CP_UTF8) {
        return;
    }

    if (SetConsoleOutputCP(CP_UTF8) == 0) {
        std::fprintf(stderr, "切换控制台输出代码页到 UTF-8 失败, 错误码 %lu\n", GetLastError());
        return;
    }

    gPreviousOutputCodePage = currentCodePage;
    std::printf("控制台输出代码页由 %u 切换到 65001 (UTF-8)\n", currentCodePage);
}

void restoreConsoleEncoding()
{
    if (gPreviousOutputCodePage == 0) {
        return;
    }

    SetConsoleOutputCP(gPreviousOutputCodePage);
    gPreviousOutputCodePage = 0;
}
