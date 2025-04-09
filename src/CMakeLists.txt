set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)

file(GLOB_RECURSE SOURCES
    ${SOURCE_DIR}/*.cpp
)

add_executable(NDVDetector ${SOURCES})

