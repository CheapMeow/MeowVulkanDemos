#include "gpu_clock_lock.h"
#include "vk_check.h"

#define NOMINMAX
#include <windows.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

// 一次 nvidia-smi 调用的结果。launched 为 false 表示进程都没能启动，
// 通常意味着没有安装 NVIDIA 驱动或者 nvidia-smi 不在 PATH 里，这是正常的环境差异，不是程序错误
struct NvidiaSmiResult {
    bool launched = false;
    DWORD exitCode = 0;
    std::string output;
};

// 去掉字符串首尾的空白字符
std::string trim(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

// 按逗号切分 nvidia-smi 的 csv 输出行，并去掉每个字段的首尾空白
std::vector<std::string> splitCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(trim(field));
    }
    return fields;
}

// 按行切分命令输出，跳过空行，并去掉行尾的 \r
std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool isAllDigits(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// 启动 nvidia-smi 并等待结束，捕获标准输出与标准错误合并后的文本
// 管道、进程这些 Win32 资源的创建失败属于环境异常而非预期路径，直接终止
NvidiaSmiResult runNvidiaSmi(const std::string& arguments)
{
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (CreatePipe(&readPipe, &writePipe, &securityAttributes, 0) == 0) {
        FATAL("failed to create pipe for nvidia-smi, error %lu", GetLastError());
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;

    PROCESS_INFORMATION processInfo = {};

    const std::string commandLine = "nvidia-smi " + arguments;
    // CreateProcessA 会原地修改命令行缓冲区，这里用可写的 vector<char> 承载
    std::vector<char> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back('\0');

    NvidiaSmiResult result;
    const BOOL created = CreateProcessA(nullptr, commandLineBuffer.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (created == 0) {
        // 找不到 nvidia-smi.exe：没有安装 NVIDIA 驱动，或者不在 PATH 里
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return result;
    }
    CloseHandle(writePipe);

    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) != 0 && bytesRead > 0) {
        result.output.append(buffer, bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    result.launched = true;
    return result;
}

// 读取指定显卡支持的频率档位(clockType 为 "gr" 或 "mem")，按降序返回
std::vector<uint32_t> querySupportedClocksMHz(uint32_t gpuIndex, const char* clockType)
{
    std::ostringstream args;
    args << "--query-supported-clocks=" << clockType << " --format=csv,noheader --id=" << gpuIndex;
    const NvidiaSmiResult result = runNvidiaSmi(args.str());

    std::vector<uint32_t> clocks;
    if (!result.launched || result.exitCode != 0) {
        return clocks;
    }

    for (const std::string& line : splitLines(result.output)) {
        std::string value = line;
        const size_t unitPosition = value.find("MHz");
        if (unitPosition != std::string::npos) {
            value = value.substr(0, unitPosition);
        }
        value = trim(value);
        if (isAllDigits(value)) {
            clocks.push_back(static_cast<uint32_t>(std::atoi(value.c_str())));
        }
    }

    std::sort(clocks.begin(), clocks.end(), std::greater<uint32_t>());
    clocks.erase(std::unique(clocks.begin(), clocks.end()), clocks.end());
    return clocks;
}

// 两条 nvidia-smi 输出各自去掉首尾空白后用一个空格拼起来，作为诊断信息
std::string combineDetail(const std::string& first, const std::string& second)
{
    const std::string trimmedFirst = trim(first);
    const std::string trimmedSecond = trim(second);
    if (trimmedFirst.empty()) {
        return trimmedSecond;
    }
    if (trimmedSecond.empty()) {
        return trimmedFirst;
    }
    return trimmedFirst + " " + trimmedSecond;
}

} // namespace

void detectGpuClockLockState(GpuClockLockState& state)
{
    state = GpuClockLockState();

    const NvidiaSmiResult listResult = runNvidiaSmi("--query-gpu=index,name --format=csv,noheader");
    if (!listResult.launched) {
        state.lastAction = GpuClockLockAction::kDetectFailed;
        state.lastActionDetail = "nvidia-smi not found on PATH";
        return;
    }
    if (listResult.exitCode != 0) {
        state.lastAction = GpuClockLockAction::kDetectFailed;
        state.lastActionDetail = trim(listResult.output);
        return;
    }

    const std::vector<std::string> lines = splitLines(listResult.output);
    if (lines.empty()) {
        state.lastAction = GpuClockLockAction::kDetectFailed;
        state.lastActionDetail = "nvidia-smi reported no GPU";
        return;
    }

    const std::vector<std::string> fields = splitCsvLine(lines.front());
    if (fields.size() < 2) {
        state.lastAction = GpuClockLockAction::kDetectFailed;
        state.lastActionDetail = "failed to parse GPU list: " + lines.front();
        return;
    }

    state.gpuIndex = static_cast<uint32_t>(std::atoi(fields[0].c_str()));
    state.gpuName = fields[1];
    state.supportedCoreClocksMHz = querySupportedClocksMHz(state.gpuIndex, "gr");
    state.supportedMemoryClocksMHz = querySupportedClocksMHz(state.gpuIndex, "mem");

    if (state.supportedCoreClocksMHz.empty() || state.supportedMemoryClocksMHz.empty()) {
        state.lastAction = GpuClockLockAction::kDetectFailed;
        state.lastActionDetail = state.gpuName + " reports no supported clock, locking is likely unsupported";
        return;
    }

    state.detected = true;
    state.selectedCoreClockIndex = 0;   // 降序排列，下标 0 就是最高档位
    state.selectedMemoryClockIndex = 0;
    state.lastAction = GpuClockLockAction::kDetected;
}

// 读一次实时频率，成功返回真并填好两个频率值，失败时 outResult 里带着 nvidia-smi 的输出
static bool queryCurrentClocksMHz(uint32_t gpuIndex, NvidiaSmiResult& outResult, uint32_t& outCoreMHz,
                                  uint32_t& outMemoryMHz)
{
    std::ostringstream args;
    args << "-i " << gpuIndex << " --query-gpu=clocks.gr,clocks.mem --format=csv,noheader,nounits";
    outResult = runNvidiaSmi(args.str());

    const std::vector<std::string> lines = splitLines(outResult.output);
    const std::vector<std::string> fields =
        lines.empty() ? std::vector<std::string>() : splitCsvLine(lines.front());
    const bool ok = outResult.launched && outResult.exitCode == 0 && fields.size() >= 2 &&
                    isAllDigits(fields[0]) && isAllDigits(fields[1]);
    if (!ok) {
        return false;
    }

    outCoreMHz = static_cast<uint32_t>(std::atoi(fields[0].c_str()));
    outMemoryMHz = static_cast<uint32_t>(std::atoi(fields[1].c_str()));
    return true;
}

void queryLiveGpuClocks(GpuClockLockState& state)
{
    if (!state.detected) {
        return;
    }

    NvidiaSmiResult result;
    uint32_t coreMHz = 0;
    uint32_t memoryMHz = 0;
    if (queryCurrentClocksMHz(state.gpuIndex, result, coreMHz, memoryMHz)) {
        state.clocksQueried = true;
        state.currentCoreClockMHz = coreMHz;
        state.currentMemoryClockMHz = memoryMHz;
    } else {
        state.lastAction = GpuClockLockAction::kQueryFailed;
        state.lastActionDetail = combineDetail(result.output, std::string());
    }
}

static void gpuClockMonitorLoop(GpuClockMonitor& monitor, double startTime)
{
    while (true) {
        std::unique_lock<std::mutex> lock(monitor.mutex);
        monitor.wakeUp.wait_for(lock, std::chrono::seconds(GPU_CLOCK_SAMPLE_PERIOD_SECONDS),
                                [&monitor] { return monitor.stopRequested; });
        if (monitor.stopRequested) {
            return;
        }
        // 采样期间不持锁，等待 nvidia-smi 的时间不会挡住界面读取历史
        lock.unlock();

        NvidiaSmiResult result;
        uint32_t coreMHz = 0;
        uint32_t memoryMHz = 0;
        if (!queryCurrentClocksMHz(monitor.gpuIndex, result, coreMHz, memoryMHz)) {
            continue;
        }
        // 时间戳取自与耗时曲线同一个时钟，两条曲线的横轴才能对齐
        const float elapsedSeconds = static_cast<float>(glfwGetTime() - startTime);

        lock.lock();
        monitor.timeSeconds.push_back(elapsedSeconds);
        monitor.coreClockMHz.push_back(static_cast<float>(coreMHz));
        monitor.memoryClockMHz.push_back(static_cast<float>(memoryMHz));
    }
}

void startGpuClockMonitor(GpuClockMonitor& monitor, const GpuClockLockState& state, double startTime)
{
    if (!state.detected) {
        return;
    }

    monitor.detected = true;
    monitor.gpuIndex = state.gpuIndex;
    monitor.stopRequested = false;
    monitor.timeSeconds.clear();
    monitor.coreClockMHz.clear();
    monitor.memoryClockMHz.clear();
    monitor.worker = std::thread(gpuClockMonitorLoop, std::ref(monitor), startTime);
}

void stopGpuClockMonitor(GpuClockMonitor& monitor)
{
    if (!monitor.worker.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(monitor.mutex);
        monitor.stopRequested = true;
    }
    monitor.wakeUp.notify_all();
    monitor.worker.join();
}

void copyGpuClockSamples(const GpuClockMonitor& monitor, std::vector<float>& outTimeSeconds,
                         std::vector<float>& outCoreClockMHz, std::vector<float>& outMemoryClockMHz)
{
    std::lock_guard<std::mutex> lock(monitor.mutex);
    outTimeSeconds = monitor.timeSeconds;
    outCoreClockMHz = monitor.coreClockMHz;
    outMemoryClockMHz = monitor.memoryClockMHz;
}

void requestLockGpuClocks(GpuClockLockState& state)
{
    if (!state.detected) {
        return;
    }
    if (state.selectedCoreClockIndex < 0 ||
        state.selectedCoreClockIndex >= static_cast<int>(state.supportedCoreClocksMHz.size()) ||
        state.selectedMemoryClockIndex < 0 ||
        state.selectedMemoryClockIndex >= static_cast<int>(state.supportedMemoryClocksMHz.size())) {
        FATAL("gpu clock lock combo index out of range");
    }

    const uint32_t coreClockMHz = state.supportedCoreClocksMHz[state.selectedCoreClockIndex];
    const uint32_t memoryClockMHz = state.supportedMemoryClocksMHz[state.selectedMemoryClockIndex];

    std::ostringstream lockCoreArgs;
    lockCoreArgs << "-i " << state.gpuIndex << " -lgc " << coreClockMHz << "," << coreClockMHz;
    const NvidiaSmiResult coreResult = runNvidiaSmi(lockCoreArgs.str());

    std::ostringstream lockMemoryArgs;
    lockMemoryArgs << "-i " << state.gpuIndex << " -lmc " << memoryClockMHz << "," << memoryClockMHz;
    const NvidiaSmiResult memoryResult = runNvidiaSmi(lockMemoryArgs.str());

    const bool ok = coreResult.launched && coreResult.exitCode == 0 && memoryResult.launched &&
                    memoryResult.exitCode == 0;
    if (ok) {
        state.locked = true;
        state.lockedCoreClockMHz = coreClockMHz;
        state.lockedMemoryClockMHz = memoryClockMHz;
        state.lastAction = GpuClockLockAction::kLocked;
        state.lastActionDetail.clear();
    } else {
        state.lastAction = GpuClockLockAction::kLockFailed;
        state.lastActionDetail = combineDetail(coreResult.output, memoryResult.output);
    }
}

void requestUnlockGpuClocks(GpuClockLockState& state)
{
    if (!state.detected) {
        return;
    }

    std::ostringstream resetCoreArgs;
    resetCoreArgs << "-i " << state.gpuIndex << " -rgc";
    const NvidiaSmiResult coreResult = runNvidiaSmi(resetCoreArgs.str());

    std::ostringstream resetMemoryArgs;
    resetMemoryArgs << "-i " << state.gpuIndex << " -rmc";
    const NvidiaSmiResult memoryResult = runNvidiaSmi(resetMemoryArgs.str());

    state.locked = false;

    const bool ok = (coreResult.launched && coreResult.exitCode == 0) ||
                    (memoryResult.launched && memoryResult.exitCode == 0);
    if (ok) {
        state.lastAction = GpuClockLockAction::kUnlocked;
        state.lastActionDetail.clear();
    } else {
        state.lastAction = GpuClockLockAction::kUnlockFailed;
        state.lastActionDetail = combineDetail(coreResult.output, memoryResult.output);
    }
}

void releaseGpuClockLockOnExit(GpuClockLockState& state)
{
    if (state.locked) {
        requestUnlockGpuClocks(state);
    }
}
