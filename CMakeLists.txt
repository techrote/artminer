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
            /fp:precise
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
    src/core/animation.cpp
    src/core/breeding.cpp
    src/core/graph.cpp
    src/core/growth_nodes.cpp
    src/core/hash.cpp
    src/core/lineage_record.cpp
    src/core/node_registry.cpp
    src/core/parameter_locks_lineage.cpp
    src/core/prng.cpp
    src/core/recipe.cpp
    src/core/specimen_browser.cpp
)
target_include_directories(artminer_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
artminer_apply_warnings(artminer_core)

add_library(artminer_nodes STATIC
    src/nodes/growth_evaluator.cpp
    src/nodes/motion_evaluator.cpp
    src/nodes/static_evaluator.cpp
)
target_include_directories(artminer_nodes PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artminer_nodes PUBLIC artminer_core)
artminer_apply_warnings(artminer_nodes)

add_library(artminer_platform_windows STATIC
    src/platform/windows/browser_store.cpp
    src/platform/windows/lineage_store.cpp
    src/platform/windows/portable_workspace.cpp
)
target_include_directories(artminer_platform_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artminer_platform_windows PUBLIC artminer_core)
target_compile_definitions(artminer_platform_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(artminer_platform_windows)

add_library(artminer_export_windows STATIC
    src/export/export.cpp
    src/export/windows/wic_image.cpp
    src/export/windows/wic_png.cpp
)
target_include_directories(artminer_export_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artminer_export_windows PUBLIC artminer_core artminer_nodes windowscodecs ole32)
target_compile_definitions(artminer_export_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(artminer_export_windows)

add_library(artminer_gpu_windows STATIC
    src/gpu/d3d11_preview.cpp
)
target_include_directories(artminer_gpu_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artminer_gpu_windows PUBLIC artminer_core artminer_nodes d3d11 dxgi d3dcompiler)
target_compile_definitions(artminer_gpu_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(artminer_gpu_windows)

add_executable(ArtMiner
    src/app/browser_window.cpp
    src/app/export_command.cpp
    src/app/export_window.cpp
    src/app/lineage_window.cpp
    src/app/main.cpp
    src/app/playback_window.cpp
)
target_link_libraries(ArtMiner PRIVATE
    artminer_core
    artminer_nodes
    artminer_platform_windows
    artminer_export_windows
    artminer_gpu_windows
    user32
    gdi32
    comdlg32
    comctl32
)
target_compile_definitions(ArtMiner PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMiner)

add_executable(ArtMinerTests
    tests/test_main.cpp
)
target_link_libraries(ArtMinerTests PRIVATE
    artminer_core
    artminer_nodes
    artminer_platform_windows
    artminer_export_windows
    artminer_gpu_windows
)
target_compile_definitions(ArtMinerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX ARTMINER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
artminer_apply_warnings(ArtMinerTests)

add_executable(ArtMinerGpuTests
    tests/gpu_preview_test.cpp
)
target_link_libraries(ArtMinerGpuTests PRIVATE
    artminer_core
    artminer_nodes
    artminer_gpu_windows
)
target_compile_definitions(ArtMinerGpuTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX ARTMINER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
artminer_apply_warnings(ArtMinerGpuTests)

add_executable(ArtMinerSpecimenTests
    tests/specimen_browser_test.cpp
)
target_link_libraries(ArtMinerSpecimenTests PRIVATE
    artminer_core
    artminer_platform_windows
)
target_compile_definitions(ArtMinerSpecimenTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMinerSpecimenTests)

add_executable(ArtMinerGrowthTests
    tests/growth_systems_test.cpp
)
target_link_libraries(ArtMinerGrowthTests PRIVATE
    artminer_core
    artminer_nodes
)
target_compile_definitions(ArtMinerGrowthTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMinerGrowthTests)

add_executable(ArtMinerMotionTests
    tests/motion_systems_test.cpp
)
target_link_libraries(ArtMinerMotionTests PRIVATE
    artminer_core
    artminer_nodes
)
target_compile_definitions(ArtMinerMotionTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMinerMotionTests)

add_executable(ArtMinerBreedingTests
    tests/breeding_lineage_test.cpp
)
target_link_libraries(ArtMinerBreedingTests PRIVATE
    artminer_core
    artminer_nodes
    artminer_platform_windows
)
target_compile_definitions(ArtMinerBreedingTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
artminer_apply_warnings(ArtMinerBreedingTests)

add_executable(ArtMinerExportTests
    tests/export_test.cpp
)
target_link_libraries(ArtMinerExportTests PRIVATE
    artminer_core
    artminer_nodes
    artminer_export_windows
)
target_compile_definitions(ArtMinerExportTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX ARTMINER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
artminer_apply_warnings(ArtMinerExportTests)

enable_testing()
add_test(NAME artminer.unit COMMAND ArtMinerTests)
add_test(NAME artminer.gpu-preview COMMAND ArtMinerGpuTests)
add_test(NAME artminer.specimen-browser COMMAND ArtMinerSpecimenTests)
add_test(NAME artminer.growth COMMAND ArtMinerGrowthTests)
add_test(NAME artminer.motion COMMAND ArtMinerMotionTests)
add_test(NAME artminer.breeding-lineage COMMAND ArtMinerBreedingTests)
add_test(NAME artminer.export COMMAND ArtMinerExportTests)
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
add_test(
    NAME artminer.render.reference
    COMMAND ArtMiner render "${CMAKE_CURRENT_SOURCE_DIR}/examples/am003-fbm-warp.amr" "${CMAKE_CURRENT_BINARY_DIR}/ctest-am003-fbm-warp.png")
set_tests_properties(artminer.render.reference PROPERTIES PASS_REGULAR_EXPRESSION "rendered .* image-hash")
add_test(
    NAME artminer.render.growth
    COMMAND ArtMiner render "${CMAKE_CURRENT_SOURCE_DIR}/examples/am006-reaction-diffusion.amr" "${CMAKE_CURRENT_BINARY_DIR}/ctest-am006-growth.png")
set_tests_properties(artminer.render.growth PROPERTIES PASS_REGULAR_EXPRESSION "rendered .* image-hash")
add_test(
    NAME artminer.render.motion-tick
    COMMAND ArtMiner render-tick "${CMAKE_CURRENT_SOURCE_DIR}/examples/am007-particle-flow.amr" 12 "${CMAKE_CURRENT_BINARY_DIR}/ctest-am007-flow.png")
set_tests_properties(artminer.render.motion-tick PROPERTIES PASS_REGULAR_EXPRESSION "rendered tick 12 .* image-hash")
add_test(
    NAME artminer.render.feedback-tick
    COMMAND ArtMiner render-tick "${CMAKE_CURRENT_SOURCE_DIR}/examples/am007-feedback-trails.amr" 4 "${CMAKE_CURRENT_BINARY_DIR}/ctest-am007-feedback.png")
set_tests_properties(artminer.render.feedback-tick PROPERTIES PASS_REGULAR_EXPRESSION "rendered tick 4 .* image-hash")
add_test(
    NAME artminer.render.motion-range
    COMMAND ArtMiner render-range "${CMAKE_CURRENT_SOURCE_DIR}/examples/am007-particle-flow.amr" 0 2 "${CMAKE_CURRENT_BINARY_DIR}/ctest-am007-range")
set_tests_properties(artminer.render.motion-range PROPERTIES PASS_REGULAR_EXPRESSION "rendered tick 2 .* image-hash")
