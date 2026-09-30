cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This project builds with Ninja. From anywhere: mex-configure")
endif()

project(modern_exchng)

# CMake 3.28 does not enable GNU 16's C++26 dialect via CMAKE_CXX_STANDARD 26.
# Pass -std=c++26 on the command line with contracts.
set(CMAKE_CXX_EXTENSIONS OFF)

# C++26 contracts (pre / post / contract_assert). Only GCC 16+ implements them.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "Build only with GCC 16 or newer (C++26 contracts, -fcontracts). "
        "${CMAKE_CXX_COMPILER_ID} is not supported.")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR
        "Build only with GCC 16 or newer (C++26 contracts, -fcontracts). "
        "This compiler is GCC ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

file(WRITE "${CMAKE_BINARY_DIR}/mex_check_contracts.cpp"
"int Inc(int x)
  pre(x >= 0)
{
  return x + 1;
}
int main()
{
  return Inc(0) == 1 ? 0 : 1;
}
")
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++26 -fcontracts
            "${CMAKE_BINARY_DIR}/mex_check_contracts.cpp"
            -o "${CMAKE_BINARY_DIR}/mex_check_contracts.bin"
    RESULT_VARIABLE MEX_CONTRACTS_COMPILE_RESULT
    OUTPUT_VARIABLE MEX_CONTRACTS_COMPILE_OUT
    ERROR_VARIABLE MEX_CONTRACTS_COMPILE_ERR
)
if(NOT MEX_CONTRACTS_COMPILE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "This GCC does not compile C++26 contracts with -fcontracts. "
        "Use the latest GCC that supports contracts (16+).\n"
        "${MEX_CONTRACTS_COMPILE_OUT}${MEX_CONTRACTS_COMPILE_ERR}")
endif()

add_compile_options(-std=c++26 -fcontracts)
add_link_options(-fcontracts)

option(MEX_ENABLE_ASAN "Compile and link with AddressSanitizer" OFF)
option(MEX_ENABLE_UBSAN "Compile and link with UndefinedBehaviorSanitizer" OFF)

set(MEX_SANITIZER_LIST "")
if(MEX_ENABLE_ASAN)
    list(APPEND MEX_SANITIZER_LIST address)
    add_compile_options(-fno-omit-frame-pointer)
endif()
if(MEX_ENABLE_UBSAN)
    list(APPEND MEX_SANITIZER_LIST undefined)
    add_compile_options(-fno-sanitize-recover=undefined)
endif()
if(MEX_SANITIZER_LIST)
    list(JOIN MEX_SANITIZER_LIST "," MEX_SANITIZER_CSV)
    add_compile_options(-fsanitize=${MEX_SANITIZER_CSV})
    add_link_options(-fsanitize=${MEX_SANITIZER_CSV})
endif()

add_library(mex STATIC src/book.cpp)
target_include_directories(mex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(mex PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

find_package(GTest REQUIRED)

add_executable(test_book
    tests/book_test.cpp
    tests/book_alloc_test.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/book_alloc_test.cpp
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
