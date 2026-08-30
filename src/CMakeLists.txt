add_library(simulation_of_sins_game STATIC
    game/foundation_world.c
    game/foundation_world.h
    game/frame_timing.c
    game/frame_timing.h
    game/rebase_policy.c
    game/rebase_policy.h)
target_include_directories(simulation_of_sins_game
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(simulation_of_sins_game PUBLIC laiue::world)
sos_configure_c_target(simulation_of_sins_game)

add_executable(simulation_of_sins WIN32
    main_windows.c
    app/application.c
    app/application.h)
target_include_directories(simulation_of_sins
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(simulation_of_sins PRIVATE
    simulation_of_sins_game
    laiue::window
    laiue::input
    laiue::content
    laiue::world
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
