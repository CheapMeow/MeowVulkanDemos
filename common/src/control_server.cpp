#include "control_server.h"

#ifdef __ANDROID__
#include <android/log.h>
#define CS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "MeowVulkanDemo", __VA_ARGS__)
#else
#define CS_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>

#include <cstdio>
#include <cstddef>

namespace {

constexpr int kReceiveTimeoutMilliseconds = 200;

#ifdef _WIN32
using SocketFd = SOCKET;
constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketFd fd)
{
    closesocket(fd);
}
int lastSocketError()
{
    return WSAGetLastError();
}
#else
using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;
void closeSocket(SocketFd fd)
{
    ::close(fd);
}
int lastSocketError()
{
    return errno;
}
#endif

bool setReceiveTimeout(SocketFd fd, int timeoutMilliseconds)
{
    timeval timeout = {};
    timeout.tv_sec = timeoutMilliseconds / 1000;
    timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                      sizeof(timeout)) == 0;
}

// 最多等 timeoutMilliseconds 毫秒直到监听套接字上有新连接。
// accept() 阻塞时无法被另一线程的 close() 可靠地打断（Linux/安卓上的行为），
// 停止流程依赖这里的超时返回去检查 stopRequested_
bool waitForAcceptable(SocketFd fd, int timeoutMilliseconds)
{
#ifdef _WIN32
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    timeval timeout = {};
    timeout.tv_sec = timeoutMilliseconds / 1000;
    timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
    return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
#else
    pollfd descriptor = {};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    return ::poll(&descriptor, 1, timeoutMilliseconds) > 0;
#endif
}

}  // namespace

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::start(uint16_t port)
{
    if (started_) {
        return true;
    }

#ifdef _WIN32
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif

    listenerFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenerFd_ == kInvalidSocket) {
        CS_LOG("control server: socket failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    int enabled = 1;
    setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
               sizeof(enabled));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    // 只监听回环地址：控制接口不接受任何局域网或外部连接
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(listenerFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CS_LOG("control server: bind port %u failed, error %d\n", static_cast<unsigned>(port),
               lastSocketError());
        closeSocket(listenerFd_);
        listenerFd_ = kInvalidSocket;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    if (listen(listenerFd_, 4) != 0) {
        CS_LOG("control server: listen failed, error %d\n", lastSocketError());
        closeSocket(listenerFd_);
        listenerFd_ = kInvalidSocket;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    started_ = true;
    CS_LOG("control server: listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    listenerThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void ControlServer::stop()
{
    if (!started_) {
        return;
    }
    stopRequested_ = true;
    queueCondition_.notify_all();

    if (listenerFd_ != kInvalidSocket) {
        closeSocket(listenerFd_);
        listenerFd_ = kInvalidSocket;
    }
    if (listenerThread_.joinable()) {
        listenerThread_.join();
    }

    for (std::thread& thread : connectionThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    connectionThreads_.clear();

#ifdef _WIN32
    WSACleanup();
#endif
    started_ = false;
}

void ControlServer::acceptLoop()
{
    while (!stopRequested_) {
        if (!waitForAcceptable(listenerFd_, kReceiveTimeoutMilliseconds)) {
            continue;
        }
        sockaddr_in clientAddress = {};
        socklen_t clientLength = sizeof(clientAddress);
        const SocketFd clientFd = accept(listenerFd_, reinterpret_cast<sockaddr*>(&clientAddress),
                                         &clientLength);
        if (clientFd == kInvalidSocket) {
            continue;
        }

        setReceiveTimeout(clientFd, kReceiveTimeoutMilliseconds);

        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connectionThreads_.push_back(std::thread([this, clientFd] { serveConnection(clientFd); }));
    }
}

void ControlServer::serveConnection(uint64_t socketFd)
{
    std::string buffer;
    std::vector<char> chunk(4096);

    while (!stopRequested_) {
#ifdef _WIN32
        const int received = recv(static_cast<SOCKET>(socketFd), chunk.data(),
                                  static_cast<int>(chunk.size()), 0);
#else
        const ssize_t received = recv(static_cast<int>(socketFd), chunk.data(), chunk.size(), 0);
#endif
        if (received > 0) {
            buffer.append(chunk.data(), static_cast<size_t>(received));

            size_t newline = 0;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }

                PendingRequest request;
                request.line = line;
                std::future<std::string> replyFuture = request.reply.get_future();

                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    pendingRequests_.push_back(std::move(request));
                }
                queueCondition_.notify_one();

                const std::string reply = replyFuture.get();
                sendLine(socketFd, reply);
            }
        } else {
            // 连接关闭或接收超时，超时期间检查是否收到停止请求
            if (received == 0) {
                break;
            }
            if (stopRequested_) {
                break;
            }
        }
    }

#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socketFd));
#else
    ::close(static_cast<int>(socketFd));
#endif
}

void ControlServer::sendLine(uint64_t socketFd, const std::string& text)
{
    std::string payload = text;
    payload.push_back('\n');
#ifdef _WIN32
    send(static_cast<SOCKET>(socketFd), payload.data(), static_cast<int>(payload.size()), 0);
#else
    send(static_cast<int>(socketFd), payload.data(), payload.size(), 0);
#endif
}

void ControlServer::pump()
{
    std::deque<PendingRequest> batch;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        batch.swap(pendingRequests_);
    }

    for (PendingRequest& request : batch) {
        std::string reply;
        if (onCommand) {
            reply = onCommand(request.line);
        }
        if (reply.empty()) {
            reply = "ok";
        }
        request.reply.set_value(reply);
    }
}
