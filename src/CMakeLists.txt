file(GLOB_RECURSE CPP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.ui"
)

set(SOURCES
    ${CPP_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/libplacebo_libav.c
)

qt_add_executable(NDVDetector
    MANUAL_FINALIZATION
    ${SOURCES}
)

target_include_directories(NDVDetector PRIVATE
    ${LIBAV_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/vendor/phash/third-party/CImg
    ${CMAKE_SOURCE_DIR}/vendor/hftrie/include
)

target_link_libraries(NDVDetector PRIVATE
    ${LIBAV_LDFLAGS}
    PkgConfig::PLACEBO
    pHash
    SQLite::SQLite3
    hftrie
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::Gui
    spdlog::spdlog
    nlohmann_json::nlohmann_json   
)

