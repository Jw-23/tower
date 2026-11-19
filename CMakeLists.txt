cmake_minimum_required(VERSION 3.14)
project(tower VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED)

add_library(tower INTERFACE)
add_library(tower::tower ALIAS tower)
target_compile_features(tower INTERFACE cxx_std_14)
target_include_directories(tower INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tower INTERFACE Boost::boost)

include(CTest)
option(TOWER_BUILD_BENCHMARKS "Build the tower microbenchmarks" OFF)
option(TOWER_BUILD_EXAMPLES "Build the tower business examples" OFF)
if(BUILD_TESTING OR TOWER_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(BUILD_TESTING)
    set(BUILD_GMOCK OFF)
    set(INSTALL_GTEST OFF)
    set(gtest_force_shared_crt ON)
    FetchContent_Declare(googletest
        URL https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.16.0
        DOWNLOAD_NAME googletest-1.16.0.tar.gz
        URL_HASH SHA256=78c676fc63881529bf97bf9d45948d905a66833fbfa5318ea2cd7478cb98f399)
    FetchContent_MakeAvailable(googletest)

    add_executable(tower_tests tests/tower_tests.cpp tests/dependency_fixture.cpp)
    target_link_libraries(tower_tests PRIVATE tower::tower GTest::gtest_main)
    set_target_properties(tower_tests gtest gtest_main PROPERTIES
        CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$" AND NOT MSVC)
        foreach(target IN ITEMS tower_tests gtest gtest_main)
            target_compile_options(${target} PRIVATE -fno-exceptions)
        endforeach()
    endif()
    include(GoogleTest)
    gtest_discover_tests(tower_tests)
endif()

if(TOWER_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF)
    set(BENCHMARK_ENABLE_INSTALL OFF)
    set(BENCHMARK_ENABLE_WERROR OFF)
    FetchContent_Declare(benchmark
        URL https://codeload.github.com/google/benchmark/tar.gz/refs/tags/v1.9.0
        DOWNLOAD_NAME benchmark-1.9.0.tar.gz
        URL_HASH SHA256=35a77f46cc782b16fac8d3b107fbfbb37dcd645f7c28eee19f3b8e0758b48994)
    FetchContent_MakeAvailable(benchmark)

    add_executable(tower_benchmarks benchmarks/tower_benchmarks.cpp benchmarks/business_benchmarks.cpp)
    target_include_directories(tower_benchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/example")
    target_link_libraries(tower_benchmarks PRIVATE tower::tower benchmark::benchmark_main)
    set_target_properties(tower_benchmarks PROPERTIES
        CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$" AND NOT MSVC)
        target_compile_options(tower_benchmarks PRIVATE -fno-exceptions)
    endif()
endif()

if(TOWER_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

