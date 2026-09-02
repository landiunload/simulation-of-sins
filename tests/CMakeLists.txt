add_executable(simulation_of_sins_game_test game_test.c)
target_link_libraries(simulation_of_sins_game_test PRIVATE
    simulation_of_sins_core)
if(NOT WIN32)
    target_link_libraries(simulation_of_sins_game_test PRIVATE m)
endif()
sos_configure_c_target(simulation_of_sins_game_test)
sos_deploy_runtime(simulation_of_sins_game_test laiue::world)

add_test(NAME simulation_of_sins.game
    COMMAND simulation_of_sins_game_test)
set_tests_properties(simulation_of_sins.game PROPERTIES TIMEOUT 30)

if(SOS_BUILD_HEADLESS)
    add_test(NAME simulation_of_sins.bootstrap
        COMMAND simulation_of_sins_headless --smoke)
    set_tests_properties(simulation_of_sins.bootstrap PROPERTIES TIMEOUT 30)
endif()

if(SOS_BUILD_CLIENT)
    add_test(NAME simulation_of_sins.client_bootstrap
        COMMAND simulation_of_sins --smoke)
    add_test(NAME simulation_of_sins.render
        COMMAND simulation_of_sins --render-smoke)
    add_test(NAME simulation_of_sins.rebase_render
        COMMAND simulation_of_sins --rebase-render-smoke)
    set_tests_properties(
        simulation_of_sins.client_bootstrap
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
endif()
