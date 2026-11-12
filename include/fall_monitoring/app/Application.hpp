#pragma once

#include <memory>

namespace fall_monitoring::app
{
    /**
     * @brief 组合根：仅负责组件组装及生命周期，不处理操作系统信号。
     */
    class Application final 
    {
    public:
        Application();
        ~Application();
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        [[nodiscard]] bool init(int argc, char* argv[]);
        [[nodiscard]] bool start();
        [[nodiscard]] bool tick();  // 返回 false 表示核心运行故障，main 应以非零退出码结束。
        void stop() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}