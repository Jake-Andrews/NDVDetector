file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(NDVDetector ${SOURCES})

target_include_directories(NDVDetector PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/phash/third-party/CImg
)

target_link_libraries(NDVDetector PRIVATE
    PkgConfig::LIBAV
    pHash
    SQLite::SQLite3
    hftrie
)

