cmake_minimum_required(VERSION 3.28)

project(simulation_of_sins
    VERSION 0.1.0
    DESCRIPTION "Simulation of sins game"
    LANGUAGES C)

option(SOS_BUILD_BENCHMARKS "Build manual performance benchmarks" OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "Simulation of sins currently requires the Windows graphics SDK")
endif()
if(NOT MSVC)
    message(FATAL_ERROR
        "Simulation of sins requires the MSVC ABI (MSVC or clang-cl)")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Simulation of sins supports only x86_64")
endif()
string(TOLOWER "${CMAKE_C_COMPILER_ARCHITECTURE_ID}" SOS_COMPILER_ARCHITECTURE)
if(NOT SOS_COMPILER_ARCHITECTURE MATCHES "^(x64|amd64|x86_64)$")
    message(FATAL_ERROR
        "Simulation of sins supports only x86_64; detected "
        "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
endif()

# Keep distributable game binaries independent of an installed VC Runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(SimulationTarget)
include(RuntimeDeployment)

find_package(laiue 0.7.0 EXACT CONFIG REQUIRED)

set(SOS_REQUIRED_ENGINE_TARGETS
    laiue::window
    laiue::input
    laiue::content
    laiue::world
    laiue::mesher
    laiue::render
    laiue::scene)
foreach(engine_target IN LISTS SOS_REQUIRED_ENGINE_TARGETS)
    if(NOT TARGET "${engine_target}")
        message(FATAL_ERROR
            "The selected laiue SDK does not provide ${engine_target}")
    endif()
endforeach()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

include(CTest)
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(SOS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

set(SOS_BUNDLE_DIRECTORY
    "${CMAKE_BINARY_DIR}/bundles/SimulationOfSins/$<CONFIG>")
add_custom_target(simulation_of_sins_bundle
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SOS_BUNDLE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_BUNDLE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:simulation_of_sins>"
        $<TARGET_RUNTIME_DLLS:simulation_of_sins>
        "${SOS_BUNDLE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets"
        "${SOS_BUNDLE_DIRECTORY}"
    DEPENDS simulation_of_sins_runtime
    COMMAND_EXPAND_LISTS
    COMMENT "Staging the Simulation of sins bundle"
    VERBATIM)
