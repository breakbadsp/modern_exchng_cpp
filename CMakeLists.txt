cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This project builds with Ninja. From anywhere: mex-configure")
endif()

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

find_package(GTest REQUIRED)

add_executable(test_book
    tests/book_test.cpp
    tests/exchange_session_test.cpp
)
target_link_libraries(test_book PRIVATE mex GTest::gtest_main)
target_include_directories(test_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_options(test_book PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

enable_testing()
add_test(NAME test_book COMMAND test_book)
