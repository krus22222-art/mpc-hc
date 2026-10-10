cmake_minimum_required(VERSION 3.21)
project(SeriesShare LANGUAGES CXX)

# Dependencies come from vcpkg (triplet x64-windows-static), see BUILD.md:
#   libdatachannel[srtp,ws] (Mbed TLS backend, overlay ports in vcpkg-overlay/), opus, libyuv, nlohmann-json
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# libdatachannel was built against Mbed TLS and expects this umbrella target
find_package(MbedTLS CONFIG REQUIRED)
if(NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED)
    target_link_libraries(MbedTLS::MbedTLS INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

find_package(LibDataChannel CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# The viewer page is embedded into the executable.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/viewer.html" VIEWER_HTML)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/viewer_html.h.in" "${CMAKE_CURRENT_BINARY_DIR}/viewer_html.h" @ONLY)

add_executable(mpc-share WIN32
    main.cpp
    VideoCapture.cpp
    AudioCapture.cpp
    Streamer.cpp
    HttpServer.cpp
    RelayClient.cpp
    mpc-share.rc
    mpc-share.manifest
)
target_include_directories(mpc-share PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(mpc-share PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mpc-share PRIVATE /W3 /utf-8 /EHsc /bigobj)
target_link_libraries(mpc-share PRIVATE
    LibDataChannel::LibDataChannel
    Opus::opus
    yuv
    nlohmann_json::nlohmann_json
    d3d11 dxgi dwmapi ws2_32 mmdevapi ole32 oleaut32 windowsapp comctl32 shlwapi shell32
    mfplat mfuuid wmcodecdspuuid strmiids crypt32 bcrypt
)
