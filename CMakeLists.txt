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
add_compile_definitions(CL_TARGET_OPENCL_VERSION=300)

set(PHASH_DYNAMIC OFF CACHE BOOL "")
set(PHASH_STATIC ON CACHE BOOL "")
set(PHASH_BINDINGS OFF CACHE BOOL "")
set(PHASH_EXAMPLES OFF CACHE BOOL "")
set(WITH_AUDIO_HASH OFF CACHE BOOL "")
set(WITH_VIDEO_HASH OFF CACHE BOOL "")

# Local FFmpeg
set(FFMPEG_ROOT "$ENV{HOME}/ffmpeg_build")
set(CMAKE_PREFIX_PATH "${FFMPEG_ROOT}")
set(CMAKE_INCLUDE_PATH "${FFMPEG_ROOT}/include")
set(CMAKE_LIBRARY_PATH "${FFMPEG_ROOT}/lib")
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig")

# FFmpeg components
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavdevice libavfilter libavformat libavcodec
    libswresample libswscale libavutil
)

pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

# GLEW static build (EGL-only, no GLX)
set(GLEW_ROOT "$ENV{HOME}/opt/glew-egl/install")
add_library(GLEW::GLEW STATIC IMPORTED)
set_target_properties(GLEW::GLEW PROPERTIES
    IMPORTED_LOCATION             "${GLEW_ROOT}/lib64/libGLEW.a"
    INTERFACE_INCLUDE_DIRECTORIES "${GLEW_ROOT}/include"
)

# Remaining packages
find_package(OpenGL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui OpenGL)
qt_standard_project_setup()

add_subdirectory(vendor/phash)
add_subdirectory(vendor/hftrie)
add_subdirectory(src)

