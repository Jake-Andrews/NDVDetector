file(GLOB_RECURSE CPP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.ui"
)

qt_add_executable(NDVDetector
    MANUAL_FINALIZATION
    ${CPP_SOURCES}
)

target_include_directories(NDVDetector PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/phash/third-party/CImg
    ${CMAKE_SOURCE_DIR}/vendor/hftrie/include
    ${FFMPEG_ROOT}/include
)

target_link_libraries(NDVDetector PRIVATE
    PkgConfig::AVUTIL
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVFILTER
    PkgConfig::SWSCALE
    PkgConfig::AVDEVICE

    pHash
    hftrie
    SQLite::SQLite3
    spdlog::spdlog
    nlohmann_json::nlohmann_json

    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Concurrent
)

qt_finalize_executable(NDVDetector)

