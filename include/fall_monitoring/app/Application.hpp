#pragma once

namespace fall_monitoring::app
{
    /**
     * @brief 边缘监测应用
     * 
     * 1.组织系统各模块的初始化
     * 2.管理系统的启动和停止
     * 3.协调应用层服务的生命周期
     */
    class Application
    {
    public:
        // 启动应用，返回进程退出码
        int run();
    };
} // namespace fall_monitoring::app