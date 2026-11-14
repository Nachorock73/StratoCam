cmake_minimum_required(VERSION 3.22)
project(AndroidCamVirtualCamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(AndroidCamVirtualCamera SHARED
    VirtualCameraSource.cpp
    AndroidCamVirtualCamera.def)
target_include_directories(AndroidCamVirtualCamera PRIVATE .. ../shared)
target_compile_definitions(AndroidCamVirtualCamera PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(AndroidCamVirtualCamera PRIVATE mfplat mf mfuuid ksguid uuid ole32 propsys)
target_link_options(AndroidCamVirtualCamera PRIVATE -static)
set_target_properties(AndroidCamVirtualCamera PROPERTIES PREFIX "" OUTPUT_NAME "AndroidCamVirtualCamera")

add_executable(AndroidCamVirtualCameraManager VirtualCameraManager.cpp)
target_compile_definitions(AndroidCamVirtualCameraManager PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(AndroidCamVirtualCameraManager PRIVATE mfplat uuid ole32 propsys)
target_link_options(AndroidCamVirtualCameraManager PRIVATE -municode -static)

add_executable(AndroidCamVirtualCameraProbe VirtualCameraProbe.cpp)
target_compile_definitions(AndroidCamVirtualCameraProbe PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(AndroidCamVirtualCameraProbe PRIVATE mfreadwrite mfplat mf mfuuid uuid ole32 propsys)
target_link_options(AndroidCamVirtualCameraProbe PRIVATE -municode -static)

add_executable(AndroidCamVirtualCameraMetrics VirtualCameraMetrics.cpp)
target_include_directories(AndroidCamVirtualCameraMetrics PRIVATE .. ../shared)
target_compile_definitions(AndroidCamVirtualCameraMetrics PRIVATE UNICODE _UNICODE)
target_link_options(AndroidCamVirtualCameraMetrics PRIVATE -municode -static)
