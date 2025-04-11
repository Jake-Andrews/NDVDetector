file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(NDVDetector ${SOURCES})

target_link_libraries(NDVDetector PRIVATE
    PkgConfig::LIBAV
    pHash
)

