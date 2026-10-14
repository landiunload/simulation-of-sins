cmake_minimum_required(VERSION 4.2)

project(simulation_of_sins
    VERSION 0.1.0
    DESCRIPTION "Simulation of sins"
    LANGUAGES C)

set(SOS_ENGINE_SOURCE_DIR "" CACHE PATH
    "Optional laiue source tree for platform superbuild integration")
option(SOS_BUILD_ENGINE_TESTS
    "Build laiue's own tests when using SOS_ENGINE_SOURCE_DIR" OFF)
option(SOS_ENGINE_FETCH
    "Fetch laiue when no source tree or SDK is found" ON)
set(SOS_ENGINE_REVISION "origin/main" CACHE STRING
    "Fetched laiue Git revision; origin/main tracks development, locked uses engine.lock")
option(SOS_BUILD_HEADLESS "Build the headless simulation executable" ON)

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

# Движок и игра развиваются вместе, поэтому по умолчанию игра собирает
# соседний `../laiue` из исходников: правка движка сразу видна игре.
# Заранее собранный SDK выбирается явным `laiue_DIR` — так CI проверяет
# установленный пакет. Обе дороги дают одни и те же цели `laiue::*` и одни и
# те же пути заголовков.
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
    # сборки можно явно выбрать locked или commit SHA.
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
    set(BUILD_TESTING ${SOS_BUILD_ENGINE_TESTS})
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
        set(BUILD_TESTING ${SOS_BUILD_ENGINE_TESTS})
        add_subdirectory("${SOS_ENGINE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_engine")
        set(BUILD_TESTING "${SOS_GAME_BUILD_TESTING}")
    endif()
elseif(laiue_DIR)
    message(STATUS "laiue: using the prepared SDK at ${laiue_DIR}")
    find_package(laiue 0.1.0 CONFIG REQUIRED)
else()
    message(FATAL_ERROR
        "No laiue engine available. Clone https://github.com/landiunload/laiue "
        "next to this repository, or point SOS_ENGINE_SOURCE_DIR at a source "
        "tree, or laiue_DIR at a prepared SDK. Re-enable SOS_ENGINE_FETCH to "
        "let the build download SOS_ENGINE_REVISION (origin/main by default).")
endif()

if(NOT TARGET laiue::bootstrap)
    message(FATAL_ERROR "The selected laiue build does not provide laiue::bootstrap")
endif()

# The client needs a window, input and at least one graphics provider. The
# game only copies these modules next to itself and loads them at run time,
# so a build that lacks them simply has no client.
set(SOS_CLIENT_ENGINE_MODULES)
foreach(module IN ITEMS laiue::window laiue::input laiue::graphics_d3d12 laiue::graphics_vulkan)
    if(TARGET ${module})
        list(APPEND SOS_CLIENT_ENGINE_MODULES ${module})
    endif()
endforeach()
set(SOS_CLIENT_AVAILABLE OFF)
if(TARGET laiue::window AND TARGET laiue::input AND
   (TARGET laiue::graphics_d3d12 OR TARGET laiue::graphics_vulkan))
    set(SOS_CLIENT_AVAILABLE ON)
endif()
option(SOS_BUILD_CLIENT "Build the interactive client" ${SOS_CLIENT_AVAILABLE})
if(SOS_BUILD_CLIENT AND NOT SOS_CLIENT_AVAILABLE)
    message(FATAL_ERROR
        "SOS_BUILD_CLIENT needs a laiue build with window, input and a "
        "graphics provider; disable the client for a headless build")
endif()
message(STATUS "Simulation of sins client: ${SOS_BUILD_CLIENT}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Self-contained folders to hand out: the executable plus the engine modules
# it loads. Release bundles are stripped outside Windows.
if(SOS_BUILD_CLIENT)
    set(SOS_BUNDLE_DIRECTORY "${CMAKE_BINARY_DIR}/bundles/SimulationOfSins/$<CONFIG>")
    sos_release_bundle_strip_command(SOS_CLIENT_BUNDLE_STRIP_COMMAND
        "${SOS_BUNDLE_DIRECTORY}" simulation_of_sins)
    get_target_property(SOS_CLIENT_MODULE_FILES simulation_of_sins SOS_ENGINE_MODULE_FILES)
    add_custom_target(simulation_of_sins_bundle
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SOS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:simulation_of_sins>" ${SOS_CLIENT_MODULE_FILES}
            "${SOS_BUNDLE_DIRECTORY}"
        ${SOS_CLIENT_BUNDLE_STRIP_COMMAND}
        DEPENDS simulation_of_sins simulation_of_sins_engine_modules
        COMMAND_EXPAND_LISTS
        COMMENT "Staging the Simulation of sins bundle"
        VERBATIM)
endif()

if(SOS_BUILD_HEADLESS)
    set(SOS_HEADLESS_BUNDLE_DIRECTORY
        "${CMAKE_BINARY_DIR}/bundles/SimulationOfSinsHeadless/$<CONFIG>")
    sos_release_bundle_strip_command(SOS_HEADLESS_BUNDLE_STRIP_COMMAND
        "${SOS_HEADLESS_BUNDLE_DIRECTORY}" simulation_of_sins_headless)
    add_custom_target(simulation_of_sins_headless_bundle
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:simulation_of_sins_headless>" "${SOS_HEADLESS_BUNDLE_DIRECTORY}"
        ${SOS_HEADLESS_BUNDLE_STRIP_COMMAND}
        DEPENDS simulation_of_sins_headless
        COMMENT "Staging the Simulation of sins headless bundle"
        VERBATIM)
endif()
