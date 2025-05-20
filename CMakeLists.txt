cmake_minimum_required(VERSION 3.31.2)

project(NDVDetector
    VERSION 1.0
    DESCRIPTION "Finds near duplicate videos"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-fmax-errors=0)

# Local FFmpeg
set(FFMPEG_ROOT "$ENV{HOME}/ffmpeg_build")
set(CMAKE_PREFIX_PATH "${FFMPEG_ROOT}")
set(CMAKE_INCLUDE_PATH "${FFMPEG_ROOT}/include")
set(CMAKE_LIBRARY_PATH "${FFMPEG_ROOT}/lib")
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig")

find_package(PkgConfig REQUIRED)

# FFmpeg components
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)

find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui Concurrent)
qt_standard_project_setup()

add_subdirectory(vendor/hftrie)
add_subdirectory(src)

