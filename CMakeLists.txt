cmake_minimum_required(VERSION 3.31.2)

project(NDVDetector
    VERSION 1.0
    DESCRIPTION "Finds Near Duplicate Videos"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-fmax-errors=0)

set(PHASH_DYNAMIC OFF CACHE BOOL "Disable shared pHash library")
set(PHASH_STATIC ON CACHE BOOL "Enable static pHash library")
set(PHASH_BINDINGS OFF CACHE BOOL "Disable pHash bindings")
set(PHASH_EXAMPLES OFF CACHE BOOL "Disable pHash examples")
set(WITH_AUDIO_HASH OFF CACHE BOOL "Disable audio hash support")
set(WITH_VIDEO_HASH OFF CACHE BOOL "Disable video hash support")

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

add_subdirectory(vendor/phash)
add_subdirectory(src)

