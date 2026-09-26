cmake_minimum_required(VERSION 3.20)

project(modern_exchng)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mex STATIC src/book.cpp)
target_include_directories(mex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(mex PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
