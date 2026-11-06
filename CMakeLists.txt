cmake_minimum_required(VERSION 3.16)

project(
    fall_monitoring_system
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fall_monitoring_system STATIC
    src/app/Application.cpp
)

# 对依赖此库的目标公开头文件目录
target_include_directories(fall_monitoring_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

#编译器警告
target_compile_options(fall_monitoring_core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(fall_monitoring_gateway
    PRIVATE
        fall_monitoring_core
)

# 启动 CTest
enable_testing()
