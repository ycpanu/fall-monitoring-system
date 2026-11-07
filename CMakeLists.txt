cmake_minimum_required(VERSION 3.16)
project(fall_monitoring_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.12.0)
    FetchContent_MakeAvailable(spdlog)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("nlohmann/json.hpp" HAVE_NLOHMANN_JSON_HEADER)
    if(NOT HAVE_NLOHMANN_JSON_HEADER)
        message(FATAL_ERROR "缺少 nlohmann/json.hpp；请安装 nlohmann-json3-dev 或设置 sysroot 包含路径")
    endif()
endif()

# 摄像头采集依赖 OpenCV；通过 -DOpenCV_DIR 指向板端 sysroot 的 CMake 包。
find_package(OpenCV REQUIRED COMPONENTS core videoio imgproc)

# RKNN SDK 隔离在 Infrastructure 层，不暴露头文件给应用层。
# 原工程依赖 sysroot/usr/include/rknn_api.h 和 sysroot/usr/lib/librknnrt.so。
# 如果使用外部交叉工具链，也可通过 -DRKNN_INCLUDE_DIR/-DRKNN_LIBRARY 指定。
find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/include")
find_library(RKNN_LIBRARY NAMES rknnrt
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib"
          "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib/aarch64-linux-gnu")
if(NOT RKNN_INCLUDE_DIR OR NOT RKNN_LIBRARY)
    message(FATAL_ERROR "找不到 RKNN SDK：请配置 RKNN_INCLUDE_DIR 与 RKNN_LIBRARY 或准备 sysroot")
endif()

add_library(fall_monitoring_core STATIC
    src/application/vision/VisionPipeline.cpp
    src/infrastructure/rknn/RknnPoseInferencer.cpp
    src/app/Application.cpp
    src/infrastructure/camera/CameraCapture.cpp
    src/infrastructure/config/ConfigManager.cpp
    src/infrastructure/logging/Logger.cpp
)

target_include_directories(fall_monitoring_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${OpenCV_INCLUDE_DIRS})
# RKNN SDK 仅用于具体实现，不能通过 PUBLIC 传播到领域与应用头文件。
target_include_directories(fall_monitoring_core PRIVATE "${RKNN_INCLUDE_DIR}")

# 业务接口不包含 SDK；在静态核心库中 PRIVATE 依赖仍会传递最终链接要求。
target_link_libraries(fall_monitoring_core
    PUBLIC spdlog::spdlog ${OpenCV_LIBS}
    PRIVATE "${RKNN_LIBRARY}")
if(nlohmann_json_FOUND)
    target_link_libraries(fall_monitoring_core PUBLIC nlohmann_json::nlohmann_json)
endif()

target_compile_definitions(fall_monitoring_core PUBLIC
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_compile_options(fall_monitoring_core PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(fall_monitoring_gateway apps/main.cpp)
target_link_libraries(fall_monitoring_gateway PRIVATE fall_monitoring_core)
