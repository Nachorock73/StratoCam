cmake_minimum_required(VERSION 3.22)
project(AndroidCamObsPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED OBS_SOURCE_DIR OR NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
    message(FATAL_ERROR "OBS_SOURCE_DIR must point to the matching OBS Studio source tree")
endif()
if(NOT DEFINED OBS_IMPORT_LIBRARY OR NOT EXISTS "${OBS_IMPORT_LIBRARY}")
    message(FATAL_ERROR "OBS_IMPORT_LIBRARY must point to the generated obs.dll import library")
endif()

set(OBS_DATA_PATH "data/libobs")
set(OBS_PLUGIN_PATH "obs-plugins/64bit")
set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file("${OBS_SOURCE_DIR}/libobs/obsconfig.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h")

add_library(android-cam-obs MODULE src/android-cam-obs.cpp)
target_include_directories(android-cam-obs PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)
target_include_directories(android-cam-obs SYSTEM PRIVATE "${OBS_SOURCE_DIR}/libobs")
target_compile_definitions(android-cam-obs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(android-cam-obs PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(android-cam-obs PRIVATE -static -static-libgcc -static-libstdc++)
target_link_libraries(android-cam-obs PRIVATE "${OBS_IMPORT_LIBRARY}" user32)
set_target_properties(android-cam-obs PROPERTIES PREFIX "" OUTPUT_NAME "stratocam-obs")
