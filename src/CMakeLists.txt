add_library(simulation_of_sins_core STATIC
    app/core_smoke.c
    app/core_smoke.h
    game/falling_cubes.c
    game/falling_cubes.h
    game/foundation_world.c
    game/foundation_world.h
    game/frame_timing.c
    game/frame_timing.h
    game/ground_provider.c
    game/ground_provider.h
    game/rebase_policy.c
    game/rebase_policy.h)
target_include_directories(simulation_of_sins_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(simulation_of_sins_core PUBLIC laiue::world laiue::physics)
sos_configure_c_target(simulation_of_sins_core)
if(NOT MSVC)
    set_target_properties(simulation_of_sins_core PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(Android|iOS|tvOS|visionOS)$" OR
   (LAIUE_PLATFORM_EXTERNAL AND
    LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC"))
    string(CONCAT _sos_static_core_link
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,simulation_of_sins_core,"
        "laiue::numeric,laiue::world,laiue::physics,laiue::content,laiue::mod>")
    string(CONCAT _sos_static_core_override
        "WHOLE_ARCHIVE,simulation_of_sins_core,"
        "laiue::numeric,laiue::world,laiue::physics,laiue::content,laiue::mod")
endif()

if(LAIUE_PLATFORM_EXTERNAL AND
   LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC" AND
   NOT CMAKE_SYSTEM_NAME MATCHES "^(Android|iOS|tvOS|visionOS)$")
    add_executable(simulation_of_sins_static_core_link_smoke
        app/apple_mobile_link_smoke.c)
    target_link_libraries(simulation_of_sins_static_core_link_smoke PRIVATE
        "${_sos_static_core_link}")
    set_property(TARGET simulation_of_sins_static_core_link_smoke PROPERTY
        LINK_LIBRARY_OVERRIDE "${_sos_static_core_override}")
    sos_configure_c_target(simulation_of_sins_static_core_link_smoke)
    if(BUILD_TESTING)
        add_test(NAME simulation_of_sins.static_core_link
            COMMAND simulation_of_sins_static_core_link_smoke)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_library(simulation_of_sins_android_core SHARED app/android_core.c)
    target_link_libraries(simulation_of_sins_android_core PRIVATE
        "${_sos_static_core_link}")
    set_property(TARGET simulation_of_sins_android_core PROPERTY
        LINK_LIBRARY_OVERRIDE "${_sos_static_core_override}")
    sos_configure_c_target(simulation_of_sins_android_core)
    set_target_properties(simulation_of_sins_android_core PROPERTIES
        C_VISIBILITY_PRESET hidden
        OUTPUT_NAME "simulation_of_sins")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|visionOS)$")
    add_executable(simulation_of_sins_apple_mobile_link_smoke MACOSX_BUNDLE
        app/apple_mobile_link_smoke.c)
    target_link_libraries(simulation_of_sins_apple_mobile_link_smoke PRIVATE
        "${_sos_static_core_link}")
    set_property(TARGET simulation_of_sins_apple_mobile_link_smoke PROPERTY
        LINK_LIBRARY_OVERRIDE "${_sos_static_core_override}")
    sos_configure_c_target(simulation_of_sins_apple_mobile_link_smoke)
    set_target_properties(simulation_of_sins_apple_mobile_link_smoke PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "SimulationOfSinsCoreLinkSmoke"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.laiue.simulation-of-sins.core-link-smoke"
        OUTPUT_NAME "SimulationOfSinsCoreLinkSmoke"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
endif()

if(SOS_BUILD_HEADLESS)
    add_executable(simulation_of_sins_headless main_headless.c)
    target_link_libraries(simulation_of_sins_headless PRIVATE
        simulation_of_sins_core)
    sos_configure_c_target(simulation_of_sins_headless)
    sos_deploy_runtime(simulation_of_sins_headless laiue::world laiue::physics)
    set_target_properties(simulation_of_sins_headless PROPERTIES
        OUTPUT_NAME "SimulationOfSinsHeadless")
endif()

if(SOS_BUILD_CLIENT)
    if(NOT WIN32)
        message(FATAL_ERROR
            "The current interactive client entry point requires Windows; "
            "this does not affect core/headless builds")
    endif()

    add_executable(simulation_of_sins WIN32
        main_windows.c
        app/application.c
        app/application.h)
    target_include_directories(simulation_of_sins
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(simulation_of_sins PRIVATE
        simulation_of_sins_core
        laiue::window
        laiue::input
        laiue::content
        laiue::mesher
        laiue::render
        laiue::scene)
    sos_configure_c_target(simulation_of_sins)
    sos_deploy_runtime(simulation_of_sins)

    set_target_properties(simulation_of_sins PROPERTIES
        OUTPUT_NAME "SimulationOfSins"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>")

    add_custom_target(simulation_of_sins_assets
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:simulation_of_sins>"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_FILE_DIR:simulation_of_sins>/mods"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_FILE_DIR:simulation_of_sins>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_FILE_DIR:simulation_of_sins>/textures"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/mods"
            "$<TARGET_FILE_DIR:simulation_of_sins>/mods"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/shaders"
            "$<TARGET_FILE_DIR:simulation_of_sins>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/textures"
            "$<TARGET_FILE_DIR:simulation_of_sins>/textures"
        COMMENT "Synchronizing managed game assets"
        VERBATIM)
    add_dependencies(simulation_of_sins simulation_of_sins_assets)
endif()
