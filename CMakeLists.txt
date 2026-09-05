cmake_minimum_required(VERSION 3.28)

project(simulation_of_sins
    VERSION 0.1.0
    DESCRIPTION "Simulation of sins game"
    LANGUAGES C)

option(SOS_BUILD_BENCHMARKS "Build manual performance benchmarks" OFF)
set(SOS_ENGINE_SOURCE_DIR "" CACHE PATH
    "Optional laiue source tree for platform superbuild integration")
option(SOS_BUILD_ENGINE_TESTS
    "Build laiue's own tests when using SOS_ENGINE_SOURCE_DIR" OFF)
option(SOS_ENGINE_FETCH
    "Fetch laiue when no source tree or SDK is found" ON)
set(SOS_ENGINE_REVISION "origin/main" CACHE STRING
    "Fetched laiue Git revision; origin/main tracks development, locked uses engine.lock")

if(WIN32)
    set(SOS_BUILD_CLIENT_DEFAULT ON)
else()
    set(SOS_BUILD_CLIENT_DEFAULT OFF)
endif()
option(SOS_BUILD_CLIENT "Build the interactive graphics client"
    ${SOS_BUILD_CLIENT_DEFAULT})
option(SOS_BUILD_HEADLESS "Build the portable core smoke executable" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Simulation of sins supports only 64-bit targets")
endif()

# Keep distributable game binaries independent of an installed VC Runtime.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(SimulationTarget)
include(RuntimeDeployment)
include(CTest)

# Движок и игра соединены исходниками, а не опубликованным пакетом,
# поэтому по умолчанию игра собирает движок сама. Заранее собранный SDK
# требовал ручного шага перед каждой сборкой и молча устаревал: заголовки
# оставались от прошлой сборки движка, и ошибка вылезала уже в компиляторе
# игры. Готовый пакет остаётся доступен — его выбирает тот, кто явно
# назвал `laiue_DIR`; так CI продолжает проверять установленный SDK.
if(NOT SOS_ENGINE_SOURCE_DIR AND NOT laiue_DIR)
    get_filename_component(SOS_ENGINE_SIBLING_DIR
        "${PROJECT_SOURCE_DIR}/../laiue" ABSOLUTE)
    if(EXISTS "${SOS_ENGINE_SIBLING_DIR}/CMakeLists.txt")
        set(SOS_ENGINE_SOURCE_DIR "${SOS_ENGINE_SIBLING_DIR}")
    endif()
endif()

if(NOT SOS_ENGINE_SOURCE_DIR AND NOT laiue_DIR AND SOS_ENGINE_FETCH)
    # Без соседнего дерева разработка тоже следует текущему движку:
    # FetchContent обновляет origin/main при configure. Для воспроизводимой
    # сборки можно явно выбрать locked или commit SHA. Локальные исходники
    # всегда главнее скачанной копии и не требуют Git-коммита для пересборки.
    include(EngineRevision)
    sos_resolve_engine_revision("${SOS_ENGINE_REVISION}"
        "${PROJECT_SOURCE_DIR}/engine.lock" SOS_ENGINE_FETCH_REVISION)
    message(STATUS
        "laiue: no source tree beside the game, fetching ${SOS_ENGINE_FETCH_REVISION}")

    include(FetchContent)
    FetchContent_Declare(laiue
        GIT_REPOSITORY "https://github.com/landiunload/laiue.git"
        GIT_TAG "${SOS_ENGINE_FETCH_REVISION}")
    set(SOS_GAME_BUILD_TESTING "${BUILD_TESTING}")
    if(SOS_BUILD_ENGINE_TESTS)
        set(BUILD_TESTING ON)
    else()
        set(BUILD_TESTING OFF)
    endif()
    FetchContent_MakeAvailable(laiue)
    set(BUILD_TESTING "${SOS_GAME_BUILD_TESTING}")
    set(SOS_ENGINE_SOURCE_DIR "${laiue_SOURCE_DIR}")
    set(SOS_ENGINE_ALREADY_ADDED ON)
endif()

if(SOS_ENGINE_SOURCE_DIR)
    get_filename_component(SOS_ENGINE_SOURCE_DIR
        "${SOS_ENGINE_SOURCE_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${SOS_ENGINE_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "SOS_ENGINE_SOURCE_DIR is not a laiue source tree: "
            "${SOS_ENGINE_SOURCE_DIR}")
    endif()
    message(STATUS "laiue: building from sources at ${SOS_ENGINE_SOURCE_DIR}")
    if(NOT SOS_ENGINE_ALREADY_ADDED)
        set(SOS_GAME_BUILD_TESTING "${BUILD_TESTING}")
        if(SOS_BUILD_ENGINE_TESTS)
            set(BUILD_TESTING ON)
        else()
            set(BUILD_TESTING OFF)
        endif()
        add_subdirectory("${SOS_ENGINE_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/_engine")
        set(BUILD_TESTING "${SOS_GAME_BUILD_TESTING}")
    endif()
elseif(laiue_DIR)
    message(STATUS "laiue: using the prepared SDK at ${laiue_DIR}")
    find_package(laiue 0.7.0 EXACT CONFIG REQUIRED)
else()
    message(FATAL_ERROR
        "No laiue engine available. Clone https://github.com/landiunload/laiue "
        "next to this repository, or point SOS_ENGINE_SOURCE_DIR at a source "
        "tree, or laiue_DIR at a prepared SDK. Re-enable SOS_ENGINE_FETCH to "
        "let the build download SOS_ENGINE_REVISION (origin/main by default).")
endif()

set(SOS_REQUIRED_CORE_TARGETS laiue::world laiue::physics)
foreach(engine_target IN LISTS SOS_REQUIRED_CORE_TARGETS)
    if(NOT TARGET "${engine_target}")
        message(FATAL_ERROR
            "The selected laiue core SDK does not provide ${engine_target}")
    endif()
endforeach()

if(SOS_BUILD_CLIENT)
    set(SOS_REQUIRED_CLIENT_TARGETS
        laiue::window
        laiue::input
        laiue::content
        laiue::mesher
        laiue::render
        laiue::scene)
    foreach(engine_target IN LISTS SOS_REQUIRED_CLIENT_TARGETS)
        if(NOT TARGET "${engine_target}")
            message(FATAL_ERROR
                "SOS_BUILD_CLIENT requires a laiue graphics SDK with "
                "${engine_target}; disable the client for a core-only build")
        endif()
    endforeach()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(SOS_BUILD_BENCHMARKS)
    if(NOT WIN32)
        message(FATAL_ERROR
            "The manual affinity benchmark currently supports Windows only")
    endif()
    add_subdirectory(benchmarks)
endif()

if(SOS_BUILD_CLIENT)
    set(SOS_BUNDLE_DIRECTORY
        "${CMAKE_BINARY_DIR}/bundles/SimulationOfSins/$<CONFIG>")
    sos_release_bundle_strip_command(
        SOS_CLIENT_BUNDLE_STRIP_COMMAND
        "${SOS_BUNDLE_DIRECTORY}"
        simulation_of_sins)
    add_custom_target(simulation_of_sins_bundle
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SOS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:simulation_of_sins>"
            $<$<BOOL:${WIN32}>:$<TARGET_RUNTIME_DLLS:simulation_of_sins>>
            "${SOS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "${SOS_BUNDLE_DIRECTORY}"
        ${SOS_CLIENT_BUNDLE_STRIP_COMMAND}
        DEPENDS simulation_of_sins_runtime
        COMMAND_EXPAND_LISTS
        COMMENT "Staging the Simulation of sins bundle"
        VERBATIM)
endif()

if(SOS_BUILD_HEADLESS)
    get_target_property(SOS_HEADLESS_WORLD_ALIASED_TARGET
        laiue::world ALIASED_TARGET)
    if(SOS_HEADLESS_WORLD_ALIASED_TARGET)
        set(SOS_HEADLESS_WORLD_INSPECTED_TARGET
            "${SOS_HEADLESS_WORLD_ALIASED_TARGET}")
    else()
        set(SOS_HEADLESS_WORLD_INSPECTED_TARGET laiue::world)
    endif()
    get_target_property(SOS_HEADLESS_WORLD_TARGET_TYPE
        "${SOS_HEADLESS_WORLD_INSPECTED_TARGET}" TYPE)
    set(SOS_HEADLESS_BUNDLE_RUNTIME_FILES)
    if(SOS_HEADLESS_WORLD_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
       SOS_HEADLESS_WORLD_TARGET_TYPE STREQUAL "MODULE_LIBRARY")
        list(APPEND SOS_HEADLESS_BUNDLE_RUNTIME_FILES
            "$<TARGET_FILE:laiue::world>")
    endif()

    set(SOS_HEADLESS_BUNDLE_DIRECTORY
        "${CMAKE_BINARY_DIR}/bundles/SimulationOfSinsHeadless/$<CONFIG>")
    sos_release_bundle_strip_command(
        SOS_HEADLESS_BUNDLE_STRIP_COMMAND
        "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        simulation_of_sins_headless)
    add_custom_target(simulation_of_sins_headless_bundle
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:simulation_of_sins_headless>"
            ${SOS_HEADLESS_BUNDLE_RUNTIME_FILES}
            "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "${SOS_HEADLESS_BUNDLE_DIRECTORY}/assets"
        ${SOS_HEADLESS_BUNDLE_STRIP_COMMAND}
        DEPENDS simulation_of_sins_headless_runtime
        COMMAND_EXPAND_LISTS
        COMMENT "Staging the portable Simulation of sins headless bundle"
        VERBATIM)
endif()
