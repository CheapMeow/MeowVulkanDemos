#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    // 自动退出秒数，为 0 表示一直运行到窗口关闭
    double autoExitSeconds = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        }
    }

    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "glfwInit 失败\n");
        return EXIT_FAILURE;
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        std::fprintf(stderr, "当前环境没有可用的 Vulkan 加载器\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Indirect Draw Demo", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "创建窗口失败\n");
        return EXIT_FAILURE;
    }

    const double startTime = glfwGetTime();
    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();
        if (autoExitSeconds > 0.0 && glfwGetTime() - startTime >= autoExitSeconds) {
            break;
        }
    }

    std::printf("窗口正常运行并退出\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
