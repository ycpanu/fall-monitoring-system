#include "fall_monitoring/app/Application.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

namespace 
{
    volatile std::sig_atomic_t g__keepRunning = 1;

    void onStopSignal(int) noexcept
    {
        g__keepRunning = 0;
    }
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    fall_monitoring::app::Application app;
    if (!app.init(argc, argv))
    {
        return EXIT_FAILURE;
    }

    if (!app.start())
    {
        app.stop();
        return EXIT_FAILURE;
    }

    bool fatal = false;
    while (g__keepRunning != 0)
    {
        if (!app.tick())
        {
            fatal = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    app.stop();
    return fatal ? EXIT_FAILURE : EXIT_SUCCESS;
}