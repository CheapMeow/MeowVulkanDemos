#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 回环 TCP 控制服务：测试脚本连接后按行发命令，处理函数在主线程执行，
// 避免与渲染线程争抢状态，执行完再回一行结果
class ControlServer {
public:
    // 处理函数在主线程执行，参数是去掉换行的一条命令，返回值是回给客户端的文本
    std::function<std::string(const std::string&)> onCommand;

    // 成功返回 true。bind/listen 失败时返回 false，内部已把错误写进日志
    bool start(uint16_t port);
    void stop();

    // 主线程每帧调用：把队列里的命令交给 onCommand 处理并唤醒等待的客户端线程
    void pump();

    ~ControlServer();

private:
    struct PendingRequest {
        std::string line;
        std::promise<std::string> reply;
    };

    void acceptLoop();
    void serveConnection(uint64_t socketFd);
    void sendLine(uint64_t socketFd, const std::string& text);

    bool started_ = false;
    std::atomic<bool> stopRequested_{ false };
    uint64_t listenerFd_ = 0;

    std::thread listenerThread_;
    std::vector<std::thread> connectionThreads_;
    std::mutex connectionsMutex_;

    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<PendingRequest> pendingRequests_;
};
