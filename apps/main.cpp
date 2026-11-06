#include "fall_monitoring/app/Application.hpp"

int main(int argc, char* argv[])
{
    fall_monitoring::app::Application app;
    return app.run(argc, argv);
}