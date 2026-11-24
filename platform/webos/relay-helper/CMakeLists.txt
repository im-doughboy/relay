cmake_minimum_required(VERSION 3.22)
project(relay-helper LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
set(CAST ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/cast)
qt_add_executable(relay-helper main.cpp
    ${CAST}/CastRelay.h ${CAST}/CastRelay.cpp
    ${CAST}/HlsJoiner.h ${CAST}/HlsJoiner.cpp
    ${CAST}/TsSegmenter.h ${CAST}/TsSegmenter.cpp)
target_include_directories(relay-helper PRIVATE ${CAST})
target_compile_options(relay-helper PRIVATE -include functional)
target_link_libraries(relay-helper PRIVATE Qt6::Core Qt6::Network)
# The TV's own libstdc++ is older than this compiler's; the package ships ours.
set_target_properties(relay-helper PROPERTIES BUILD_RPATH "$ORIGIN/lib")
