cmake_minimum_required(VERSION 3.24)

project(ArtMiner VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ArtMiner targets Windows 10/11 x64.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ArtMiner requires a 64-bit build. Configure CMake for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Keep the shipped binary independent of a separately installed VC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(ARTMINER_WARNINGS_AS_ERRORS "Treat ArtMiner compiler warnings as errors" ON)

function(artminer_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<BOOL:${ARTMINER_WARNINGS_AS_ERRORS}>:/WX>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${ARTMINER_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()
endfunction()

add_library(artminer_core STATIC
    src/core/graph.cpp
    src/core/hash.cpp
    src/core/node_registry.cpp
    src/core/prng.cpp
    src/core/recipe.cpp
)
target_include_directories(artminer_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
artminer_apply_warnings(artminer_core)

add_library(artminer_platform_windows STATIC
    src/platform/windows/portable_workspace.cpp
)
target_include_directories(artminer_platform_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artminer_platform_windows PUBLIC artminer_core)
target_compile_definitions(artminer_platform_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(artminer_platform_windows)

add_executable(ArtMiner
    src/app/main.cpp
)
target_link_libraries(ArtMiner PRIVATE
    artminer_core
    artminer_platform_windows
    user32
    gdi32
)
target_compile_definitions(ArtMiner PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMiner)

add_executable(ArtMinerTests
    tests/test_main.cpp
)
target_link_libraries(ArtMinerTests PRIVATE artminer_core artminer_platform_windows)
target_compile_definitions(ArtMinerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMinerTests)

enable_testing()
add_test(NAME artminer.unit COMMAND ArtMinerTests)
add_test(NAME artminer.version COMMAND ArtMiner --version)
set_tests_properties(artminer.version PROPERTIES PASS_REGULAR_EXPRESSION "ArtMiner 0.1.0-dev")
add_test(NAME artminer.help COMMAND ArtMiner --help)
set_tests_properties(artminer.help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: ArtMiner")
add_test(NAME artminer.workspace COMMAND ArtMiner --workspace "${CMAKE_CURRENT_BINARY_DIR}/ctest-workspace" --check-workspace)
set_tests_properties(artminer.workspace PROPERTIES PASS_REGULAR_EXPRESSION "workspace ready:")
add_test(NAME artminer.recipe.validate COMMAND ArtMiner recipe validate "${CMAKE_CURRENT_SOURCE_DIR}/examples/am002-minimal.amr")
set_tests_properties(artminer.recipe.validate PROPERTIES PASS_REGULAR_EXPRESSION "valid b9aed1288926e59c8c40115733cd32bf")
add_test(NAME artminer.recipe.inspect COMMAND ArtMiner recipe inspect "${CMAKE_CURRENT_SOURCE_DIR}/examples/am002-minimal.amr")
set_tests_properties(artminer.recipe.inspect PROPERTIES PASS_REGULAR_EXPRESSION "fingerprint: b9aed1288926e59c8c40115733cd32bf")
