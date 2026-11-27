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

function(mex_check_contracts)
    file(WRITE "${CMAKE_BINARY_DIR}/mex_check_contracts.cpp"
"int Inc(int p_x)
  pre(p_x >= 0)
{
  return p_x + 1;
}
int main()
{
  return Inc(0) == 1 ? 0 : 1;
}
")
    # Use the configured compiler arguments, flags, and toolchain. Require a link
    # check even when the toolchain uses static libraries for other probes.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    try_compile(MEX_CONTRACTS_COMPILE_RESULT
        "${CMAKE_BINARY_DIR}/mex_check_contracts"
        "${CMAKE_BINARY_DIR}/mex_check_contracts.cpp"
        COMPILE_DEFINITIONS -std=c++26 -fcontracts
        LINK_OPTIONS -fcontracts
        OUTPUT_VARIABLE MEX_CONTRACTS_COMPILE_OUTPUT
    )
    if(NOT MEX_CONTRACTS_COMPILE_RESULT)
        message(FATAL_ERROR
            "This GCC does not compile and link C++26 contracts with -fcontracts. "
            "Use the latest GCC that supports contracts (16+).\n"
            "${MEX_CONTRACTS_COMPILE_OUTPUT}")
    endif()
endfunction()
mex_check_contracts()

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

# The gateway's client I/O.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

# libvsr does the replication (docs/replication.md). Set MEX_LIBVSR_SOURCE_DIR
# to build with a local checkout; otherwise CMake gets the pinned commit.
set(MEX_LIBVSR_SOURCE_DIR "" CACHE PATH "A local libvsr checkout to use")
set(VSR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VSR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(MEX_LIBVSR_SOURCE_DIR)
    add_subdirectory(${MEX_LIBVSR_SOURCE_DIR} ${CMAKE_BINARY_DIR}/libvsr EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(libvsr
        GIT_REPOSITORY https://github.com/breakbadsp/libvsr.git
        GIT_TAG 0a2a771083a92d46035df17d0ee6a0bd62fe8bd5
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(libvsr)
endif()

# Apply warnings to project targets after configuring the third-party library.
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

add_library(mex STATIC src/book.cpp src/config.cpp src/engine.cpp src/gateway.cpp
    src/replication.cpp src/replicated_exchange.cpp src/market_data.cpp)
target_link_libraries(mex PUBLIC vsr_io)
target_include_directories(mex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
find_package(Threads REQUIRED)
target_link_libraries(mex PRIVATE PkgConfig::LIBURING Threads::Threads)

find_package(GTest REQUIRED)

add_executable(test_book
    tests/book_test.cpp
    tests/exchange_session_test.cpp
    tests/alloc_count_test.cpp
)
target_link_libraries(test_book PRIVATE mex GTest::gtest_main)
target_include_directories(test_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_spsc_queue tests/spsc_queue_test.cpp)
target_link_libraries(test_spsc_queue PRIVATE mex GTest::gtest_main Threads::Threads)

add_executable(test_protocol tests/protocol_test.cpp)
target_link_libraries(test_protocol PRIVATE mex GTest::gtest_main)

add_executable(test_engine tests/engine_test.cpp)
target_link_libraries(test_engine PRIVATE mex GTest::gtest_main Threads::Threads)

add_executable(test_config tests/config_test.cpp)
target_link_libraries(test_config PRIVATE mex GTest::gtest_main)
target_compile_definitions(test_config PRIVATE
    MEX_EXAMPLE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/example.cfg")

add_executable(test_replication tests/replication_test.cpp)
target_link_libraries(test_replication PRIVATE mex GTest::gtest_main)

add_executable(test_market_data_handoff tests/market_data_handoff_test.cpp)
target_link_libraries(test_market_data_handoff PRIVATE mex GTest::gtest_main Threads::Threads)

add_executable(test_gateway tests/gateway_test.cpp)
target_link_libraries(test_gateway PRIVATE mex GTest::gtest_main Threads::Threads)

# Measure the book itself, not a debug build of it. Last -O flag wins over the Debug preset.
# ignore drops contract checks so the number is the book, not the assertions.
add_executable(bench_book tests/book_bench.cpp src/book.cpp)
target_include_directories(bench_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bench_book PRIVATE -O3 -DNDEBUG -fcontract-evaluation-semantic=ignore)

# One synchronous client flow through the public Book API. -g and frame pointers
# let perf attribute samples. See docs/client-profile.md.
add_executable(bench_client tests/client_bench.cpp src/book.cpp)
target_include_directories(bench_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bench_client PRIVATE -O3 -g -DNDEBUG -fno-omit-frame-pointer
    -fcontract-evaluation-semantic=ignore)

# The exchange and the load generator are measured end to end, so they build at
# -O3 like bench_book. The exchange compiles the sources itself, because mex is
# built with the preset's flags. Contracts stay on in the exchange.
add_executable(mex_exchange src/main.cpp src/book.cpp src/config.cpp src/engine.cpp
    src/gateway.cpp src/replication.cpp src/replicated_exchange.cpp src/market_data.cpp)
target_include_directories(mex_exchange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mex_exchange PRIVATE vsr_io Threads::Threads PkgConfig::LIBURING)
target_compile_options(mex_exchange PRIVATE -O3 -DNDEBUG)

add_executable(mex_loadgen tools/loadgen.cpp)
target_link_libraries(mex_loadgen PRIVATE mex Threads::Threads)
target_compile_options(mex_loadgen PRIVATE -O3 -DNDEBUG)

enable_testing()
add_test(NAME test_book COMMAND test_book)
add_test(NAME test_spsc_queue COMMAND test_spsc_queue)
add_test(NAME test_protocol COMMAND test_protocol)
add_test(NAME test_engine COMMAND test_engine)
add_test(NAME test_config COMMAND test_config)
add_test(NAME test_replication COMMAND test_replication)
add_test(NAME test_gateway COMMAND test_gateway)
add_test(NAME test_market_data_handoff COMMAND test_market_data_handoff)
# Loopback sockets: a lost frame must fail the run, not hang it.
set_tests_properties(test_gateway test_spsc_queue test_engine test_book test_replication
    PROPERTIES TIMEOUT 120)
add_test(NAME exchange_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/exchange_smoke_test.sh
        $<TARGET_FILE:mex_exchange> $<TARGET_FILE:mex_loadgen>
        ${CMAKE_CURRENT_SOURCE_DIR}/config/example.cfg
        ${CMAKE_CURRENT_BINARY_DIR}/exchange_smoke)
set_tests_properties(exchange_smoke PROPERTIES TIMEOUT 60)
# Three replicas on loopback: failover, restart, and equal state.
add_test(NAME replicated_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/replicated_smoke_test.sh
        $<TARGET_FILE:mex_exchange> $<TARGET_FILE:mex_loadgen>
        ${CMAKE_CURRENT_SOURCE_DIR}/config/example.cfg
        ${CMAKE_CURRENT_BINARY_DIR}/replicated_smoke)
set_tests_properties(replicated_smoke PROPERTIES TIMEOUT 180)
# The flow checks its fill totals and final book state; contracts are off here.
add_test(NAME bench_client_smoke COMMAND bench_client batch 1000 64)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME market_data_live COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/market_data_test.py $<TARGET_FILE:mex_exchange>)
set_tests_properties(market_data_live PROPERTIES TIMEOUT 60)
add_test(NAME tool_limits COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_limits_test.py $<TARGET_FILE:mex_loadgen>)
add_test(NAME perf_report COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks -p "test_*.py")

file(GLOB_RECURSE MEX_FORMAT_SOURCES CONFIGURE_DEPENDS
    src/*.cpp src/*.hpp tests/*.cpp tests/*.hpp tools/*.cpp tools/*.hpp benchmarks/*.cpp)
list(APPEND MEX_FORMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/verification/TLA/matching/replay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/verification/TLA/bridge/replay.cpp)
option(MEX_REQUIRE_STYLE_TOOLS "Require Universal Ctags for the style test" OFF)
find_program(CTAGS_EXECUTABLE NAMES ctags-universal universal-ctags ctags)
if(MEX_REQUIRE_STYLE_TOOLS AND NOT CTAGS_EXECUTABLE)
    message(FATAL_ERROR "Universal Ctags is required for the TigerStyle check")
endif()
if(CTAGS_EXECUTABLE)
    add_test(NAME tiger_style COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tiger_style.py --ctags ${CTAGS_EXECUTABLE}
        ${MEX_FORMAT_SOURCES})
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
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

add_subdirectory(verification/TLA)
