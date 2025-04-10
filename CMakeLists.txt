cmake_minimum_required(VERSION 3.31.2)

project(NDVDetector
    VERSION 1.0
    DESCRIPTION "Finds Near Duplicate Videos"
    LANGUAGES CXX
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavdevice
    libavfilter
    libavformat
    libavcodec
    libswresample
    libswscale
    libavutil
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-fmax-errors=0)

add_subdirectory(src)       
