#pragma once

// 查询控制台当前的输出代码页，若不是 UTF-8 则切换过去，使中文正常显示
void configureConsoleEncoding();
// 恢复进入程序之前的输出代码页，避免影响之后在同一个控制台里执行的命令
void restoreConsoleEncoding();
