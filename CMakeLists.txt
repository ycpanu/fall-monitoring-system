cmake_minimum_required(VERSION 3.16)
project(fall_monitoring_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
find_package(ALSA REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

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
        message(FATAL_ERROR "缺少 nlohmann/json.hpp；请安装 nlohmann-json3-dev")
    endif()
endif()

# RKNN SDK，仅供基础设施实现使用。
find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/include")
find_library(RKNN_LIBRARY NAMES rknnrt
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib"
          "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib/aarch64-linux-gnu")
if(NOT RKNN_INCLUDE_DIR OR NOT RKNN_LIBRARY)
    message(FATAL_ERROR "缺少 RKNN SDK；请设置 RKNN_INCLUDE_DIR 与 RKNN_LIBRARY")
endif()

# Paho MQTT C++ 及其依赖的 C 异步客户端。
find_path(PAHO_MQTT_INCLUDE_DIR NAMES mqtt/async_client.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/include")
find_library(PAHO_MQTT_CPP_LIB NAMES paho-mqttpp3
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib")
find_library(PAHO_MQTT_C_LIB NAMES paho-mqtt3a
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/usr/lib")
if(NOT PAHO_MQTT_INCLUDE_DIR OR NOT PAHO_MQTT_CPP_LIB OR NOT PAHO_MQTT_C_LIB)
    message(FATAL_ERROR "缺少 Paho MQTT C++/C 异步库")
endif()

# sherpa-onnx C API：模型和 SDK 依赖于板端实际部署。
set(SHERPA_ONNX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx"
    CACHE PATH "sherpa-onnx SDK 根目录")
find_path(SHERPA_ONNX_INCLUDE_DIR NAMES sherpa-onnx/c-api/c-api.h
    PATHS "${SHERPA_ONNX_ROOT}/include")
find_library(SHERPA_ONNX_C_API_LIBRARY NAMES sherpa-onnx-c-api
    PATHS "${SHERPA_ONNX_ROOT}/lib")
if(NOT SHERPA_ONNX_INCLUDE_DIR OR NOT SHERPA_ONNX_C_API_LIBRARY)
    message(FATAL_ERROR "缺少 sherpa-onnx C API；请设置 SHERPA_ONNX_INCLUDE_DIR、SHERPA_ONNX_C_API_LIBRARY")
endif()
get_filename_component(SHERPA_ONNX_LIB_DIR "${SHERPA_ONNX_C_API_LIBRARY}" DIRECTORY)

add_library(fall_monitoring_core STATIC
    src/app/Application.cpp
    src/application/audio/AudioPipeline.cpp
    src/application/alarm/LocalAlarmService.cpp
    src/application/vision/VisionPipeline.cpp
    src/application/event/EventService.cpp
    src/application/runtime/RuntimeSupervisor.cpp
    src/application/network/AlertOutboxService.cpp
    src/application/network/VideoUploadService.cpp
    src/application/commands/DeviceCommandService.cpp
    src/domain/event/EventCorrelator.cpp
    src/domain/vision/PersonTracker.cpp
    src/domain/vision/FallRuleEngine.cpp
    src/infrastructure/audio/AlsaAudioCapture.cpp
    src/infrastructure/audio/SherpaKeywordSpotter.cpp
    src/infrastructure/hardware/SysfsBuzzer.cpp
    src/infrastructure/camera/CameraCapture.cpp
    src/infrastructure/config/ConfigManager.cpp
    src/infrastructure/logging/Logger.cpp
    src/infrastructure/rknn/RknnPoseInferencer.cpp
    src/infrastructure/scene/SafeZoneManager.cpp
    src/infrastructure/network/PahoMqttClient.cpp
    src/infrastructure/network/AlertJsonCodec.cpp
    src/infrastructure/network/CurlVideoUploadTransport.cpp
    src/infrastructure/system/SystemMetrics.cpp
    src/infrastructure/system/SystemdNotifier.cpp
    src/infrastructure/storage/SqliteAlertRepository.cpp
    src/infrastructure/video/FfmpegEventRecorder.cpp
    src/infrastructure/video/FfmpegLiveStreamer.cpp
)

target_include_directories(fall_monitoring_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${OpenCV_INCLUDE_DIRS})
target_include_directories(fall_monitoring_core PRIVATE
    "${RKNN_INCLUDE_DIR}"
    "${PAHO_MQTT_INCLUDE_DIR}"
    "${SHERPA_ONNX_INCLUDE_DIR}")

target_link_libraries(fall_monitoring_core
    PUBLIC spdlog::spdlog ${OpenCV_LIBS} Threads::Threads
    PRIVATE
      "${RKNN_LIBRARY}"
      "${PAHO_MQTT_CPP_LIB}" "${PAHO_MQTT_C_LIB}"
      "${SHERPA_ONNX_C_API_LIBRARY}"
      ALSA::ALSA SQLite::SQLite3 CURL::libcurl)
if(nlohmann_json_FOUND)
    target_link_libraries(fall_monitoring_core PUBLIC nlohmann_json::nlohmann_json)
endif()
target_link_options(fall_monitoring_core INTERFACE
    "-Wl,-rpath-link,${SHERPA_ONNX_LIB_DIR}")
target_compile_definitions(fall_monitoring_core PUBLIC
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
target_compile_options(fall_monitoring_core PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(fall_monitoring_gateway apps/main.cpp)
target_link_libraries(fall_monitoring_gateway PRIVATE fall_monitoring_core)
