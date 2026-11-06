cmake_minimum_required(VERSION 3.16)
project(fall_monitoring_system VERSION 0.1.0 LANGUAGES CXX)

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

add_library(fall_monitoring_core STATIC
    src/app/Application.cpp
    src/infrastructure/config/ConfigManager.cpp
    src/infrastructure/logging/Logger.cpp
)

target_include_directories(fall_monitoring_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fall_monitoring_core PUBLIC spdlog::spdlog)
if(nlohmann_json_FOUND)
    target_link_libraries(fall_monitoring_core PUBLIC nlohmann_json::nlohmann_json)
endif()

target_compile_definitions(fall_monitoring_core PUBLIC
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_compile_options(fall_monitoring_core PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(fall_monitoring_gateway apps/main.cpp)
target_link_libraries(fall_monitoring_gateway PRIVATE fall_monitoring_core)