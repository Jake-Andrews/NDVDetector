file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.ui")

qt_add_executable(NDVDetector
    MANUAL_FINALIZATION
    ${SOURCES}
)

target_include_directories(NDVDetector PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/phash/third-party/CImg
    ${CMAKE_SOURCE_DIR}/vendor/hftrie/include
)

target_link_libraries(NDVDetector PRIVATE
    PkgConfig::LIBAV
    pHash
    SQLite::SQLite3
    hftrie
    Qt6::Widgets
    spdlog::spdlog
    nlohmann_json
)

qt_finalize_executable(NDVDetector)

