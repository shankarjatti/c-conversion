project(securecom_air_maritime_ui CXX)
cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)   # Qt (built with -reduce-relocations) wants -fPIC, not CMake's -fPIE
include(cmake/LocalDeps.cmake)
add_subdirectory(air_ui)
