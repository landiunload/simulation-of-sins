# Deterministic simulation: standard C only, no engine. Everything that has to
# compute the same world on a server, every client and a replay lives here.
add_library(sos_core STATIC
    sim/beings.c
    sim/beings.h
    sim/calendar.c
    sim/calendar.h
    sim/clock.c
    sim/clock.h
    sim/random.c
    sim/random.h
    sim/space.c
    sim/space.h
    sim/terrain.c
    sim/terrain.h
    sim/world.c
    sim/world.h)
target_include_directories(sos_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT WIN32)
    target_link_libraries(sos_core PUBLIC m)
endif()
sos_configure_c_target(sos_core)

# Composition: process lifecycle, engine modules loaded by name, logging. The
# engine is reached only through the static bootstrap and service tables.
add_library(sos_app STATIC
    app/arguments.c
    app/arguments.h
    app/headless.c
    app/headless.h
    app/log.c
    app/log.h
    app/modules.c
    app/modules.h)
target_link_libraries(sos_app PUBLIC sos_core laiue::bootstrap)
sos_configure_c_target(sos_app)

if(SOS_BUILD_HEADLESS)
    add_executable(simulation_of_sins_headless app/headless_main.c)
    target_link_libraries(simulation_of_sins_headless PRIVATE sos_app)
    sos_configure_c_target(simulation_of_sins_headless)
    set_target_properties(simulation_of_sins_headless PROPERTIES
        OUTPUT_NAME "SimulationOfSinsHeadless")
endif()

if(SOS_BUILD_CLIENT)
    add_executable(simulation_of_sins WIN32
        client/client.c
        client/client.h
        client/client_main.c)
    target_link_libraries(simulation_of_sins PRIVATE sos_app)
    if(WIN32)
        target_link_libraries(simulation_of_sins PRIVATE shell32)
    endif()
    sos_configure_c_target(simulation_of_sins)
    set_target_properties(simulation_of_sins PROPERTIES
        OUTPUT_NAME "SimulationOfSins"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>")
    sos_deploy_engine_modules(simulation_of_sins ${SOS_CLIENT_ENGINE_MODULES})
endif()

# Mobile shells do not exist yet; these targets keep the whole game and the
# engine bootstrap in one final link, so a missing symbol fails CI now.
if(CMAKE_SYSTEM_NAME MATCHES "^(Android|iOS|tvOS|visionOS)$")
    set(sos_mobile_link "$<LINK_LIBRARY:WHOLE_ARCHIVE,sos_core,sos_app>")
    set(sos_mobile_link_override "WHOLE_ARCHIVE,sos_core,sos_app")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_library(simulation_of_sins_android_core SHARED app/android_core.c)
    target_link_libraries(simulation_of_sins_android_core PRIVATE
        "${sos_mobile_link}" laiue::bootstrap)
    set_property(TARGET simulation_of_sins_android_core PROPERTY
        LINK_LIBRARY_OVERRIDE "${sos_mobile_link_override}")
    sos_configure_c_target(simulation_of_sins_android_core)
    set_target_properties(simulation_of_sins_android_core PROPERTIES
        C_VISIBILITY_PRESET hidden
        OUTPUT_NAME "simulation_of_sins")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|visionOS)$")
    add_executable(simulation_of_sins_apple_mobile_link_smoke MACOSX_BUNDLE
        app/apple_mobile_link_smoke.c)
    target_link_libraries(simulation_of_sins_apple_mobile_link_smoke PRIVATE
        "${sos_mobile_link}" laiue::bootstrap)
    set_property(TARGET simulation_of_sins_apple_mobile_link_smoke PROPERTY
        LINK_LIBRARY_OVERRIDE "${sos_mobile_link_override}")
    sos_configure_c_target(simulation_of_sins_apple_mobile_link_smoke)
    set_target_properties(simulation_of_sins_apple_mobile_link_smoke PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "SimulationOfSinsCoreLinkSmoke"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.laiue.simulation-of-sins.core-link-smoke"
        OUTPUT_NAME "SimulationOfSinsCoreLinkSmoke"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
endif()
