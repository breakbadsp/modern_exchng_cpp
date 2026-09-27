cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This project builds with Ninja. From anywhere: mex-configure")
endif()

project(modern_exchng)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C++26 contracts (pre / post / contract_assert). Clang 22 does not parse them.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "This project uses C++26 contracts (-fcontracts), which GCC implements and Clang does not.")
endif()
add_compile_options(-fcontracts)
add_link_options(-fcontracts)

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

# Measure the book itself, not a debug build of it. Last -O flag wins over the Debug preset.
# ignore drops contract checks so the number is the book, not the assertions.
add_executable(bench_book tests/book_bench.cpp src/book.cpp)
target_include_directories(bench_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bench_book PRIVATE -O3 -DNDEBUG -fcontract-evaluation-semantic=ignore
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

enable_testing()
add_test(NAME test_book COMMAND test_book)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    set(MEX_FORMAT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/book.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/book.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/types.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/book_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/book_checks.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/book_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/exchange_session_test.cpp
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${MEX_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "clang-format: rewrite project C++ sources"
    )
    add_custom_target(clang-format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${MEX_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "clang-format: verify project C++ sources"
    )
else()
    message(STATUS "clang-format not found; format and clang-format-check targets omitted")
endif()
