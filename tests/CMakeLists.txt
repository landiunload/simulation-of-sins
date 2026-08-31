add_executable(simulation_of_sins_game_test game_test.c)
target_link_libraries(simulation_of_sins_game_test PRIVATE
    simulation_of_sins_game
    laiue::world)
sos_configure_c_target(simulation_of_sins_game_test)
add_dependencies(simulation_of_sins_game_test simulation_of_sins_runtime)

add_test(NAME simulation_of_sins.game
    COMMAND simulation_of_sins_game_test)
add_test(NAME simulation_of_sins.bootstrap
    COMMAND simulation_of_sins --smoke)
add_test(NAME simulation_of_sins.render
    COMMAND simulation_of_sins --render-smoke)
add_test(NAME simulation_of_sins.rebase_render
    COMMAND simulation_of_sins --rebase-render-smoke)
set_tests_properties(
    simulation_of_sins.game
    simulation_of_sins.bootstrap
    simulation_of_sins.render
    simulation_of_sins.rebase_render
    PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
        TIMEOUT 30)
set_tests_properties(simulation_of_sins.render simulation_of_sins.rebase_render
    PROPERTIES RESOURCE_LOCK d3d12)

add_custom_target(simulation_of_sins_render_smoke
    COMMAND "$<TARGET_FILE:simulation_of_sins>" --render-smoke
    DEPENDS simulation_of_sins
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
    COMMENT "Presenting three D3D12 smoke frames"
    VERBATIM)

add_custom_target(simulation_of_sins_rebase_render_smoke
    COMMAND "$<TARGET_FILE:simulation_of_sins>" --rebase-render-smoke
    DEPENDS simulation_of_sins
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
    COMMENT "Rebasing the live world and presenting three D3D12 smoke frames"
    VERBATIM)
