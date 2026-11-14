cmake_minimum_required(VERSION 3.22)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../version.properties" STRATOCAM_VERSION_LINE REGEX "^versionName=")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../version.properties" STRATOCAM_PRODUCT_LINE REGEX "^productName=")
string(REPLACE "versionName=" "" STRATOCAM_VERSION "${STRATOCAM_VERSION_LINE}")
string(REPLACE "productName=" "" STRATOCAM_PRODUCT_NAME "${STRATOCAM_PRODUCT_LINE}")
if(NOT STRATOCAM_VERSION OR NOT STRATOCAM_PRODUCT_NAME)
    message(FATAL_ERROR "version.properties must define productName and versionName")
endif()
project(StratoCamWindows VERSION ${STRATOCAM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT DEFINED OPENH264_ROOT)
    message(FATAL_ERROR "OPENH264_ROOT must point to the locally built static OpenH264 installation")
endif()
if(NOT DEFINED FFMPEG_ROOT)
    message(FATAL_ERROR "FFMPEG_ROOT must point to an FFmpeg LGPL shared development distribution")
endif()

string(REPLACE "." "," STRATOCAM_VERSION_COMMA "${STRATOCAM_VERSION}")
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/stratocam.ico" STRATOCAM_ICON_PATH)
configure_file(resources/StratoCam.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/StratoCam.rc" @ONLY)
configure_file(src/Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)

add_executable(stratocam WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/generated/StratoCam.rc"
    src/main.cpp
    src/AdbManager.cpp
    src/ControlClient.cpp
    src/DirectH264Recorder.cpp
    src/WasapiAudioCapture.cpp
    src/VideoTransport.cpp
    src/VirtualCameraFramePublisher.cpp
    src/H264Decoder.cpp
    src/ObsFramePublisher.cpp
    src/D3DRenderer.cpp
    src/AppWindow.cpp
)
set_target_properties(stratocam PROPERTIES OUTPUT_NAME "StratoCam")
target_include_directories(stratocam PRIVATE src resources "${CMAKE_CURRENT_BINARY_DIR}/generated" "${OPENH264_ROOT}/include" "${FFMPEG_ROOT}/include")
target_compile_definitions(stratocam PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(stratocam PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(stratocam PRIVATE -municode -static-libgcc -static-libstdc++)
target_link_directories(stratocam PRIVATE "${OPENH264_ROOT}/lib")
target_link_directories(stratocam PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(stratocam PRIVATE
    openh264 avformat avcodec avutil swresample mfplat mf mfuuid wmcodecdspuuid
    d3d11 dxgi d3dcompiler dwmapi uxtheme ws2_32 comctl32 gdi32 user32 shell32 ole32 uuid avrt advapi32
)

add_executable(android-cam-tests
    tests/NativeTests.cpp
    src/ControlClient.cpp
)
target_include_directories(android-cam-tests PRIVATE src "${OPENH264_ROOT}/include")
target_compile_definitions(android-cam-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(android-cam-tests PRIVATE -static-libgcc -static-libstdc++)
target_link_directories(android-cam-tests PRIVATE "${OPENH264_ROOT}/lib")
target_link_libraries(android-cam-tests PRIVATE openh264 ws2_32)

add_executable(stratocam-loopback-diagnostic
    tools/LoopbackCapture.cpp
)
set_target_properties(stratocam-loopback-diagnostic PROPERTIES OUTPUT_NAME "StratoCamLoopbackDiagnostic")
target_include_directories(stratocam-loopback-diagnostic PRIVATE src)
target_compile_definitions(stratocam-loopback-diagnostic PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(stratocam-loopback-diagnostic PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(stratocam-loopback-diagnostic PRIVATE -municode -static-libgcc -static-libstdc++)
target_link_libraries(stratocam-loopback-diagnostic PRIVATE ole32 uuid avrt)

enable_testing()
add_test(NAME native-tests COMMAND android-cam-tests)

get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(target stratocam android-cam-tests stratocam-loopback-diagnostic)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINGW_BIN}/libwinpthread-1.dll" "$<TARGET_FILE_DIR:${target}>"
    )
endforeach()

foreach(dll avformat-63.dll avcodec-63.dll avutil-61.dll swresample-7.dll)
    add_custom_command(TARGET stratocam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_ROOT}/bin/${dll}" "$<TARGET_FILE_DIR:stratocam>"
    )
endforeach()
add_custom_command(TARGET stratocam POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:stratocam>/FFMPEG-LICENSE.txt"
)
