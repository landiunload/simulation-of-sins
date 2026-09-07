# The benchmark is manual: it must stay out of the ordinary build, so the
# property is set before sos_deploy_runtime reads it.
if(WIN32)
    add_executable(simulation_of_sins_game_benchmark EXCLUDE_FROM_ALL
        game_logic_benchmark.c)
    target_link_libraries(simulation_of_sins_game_benchmark PRIVATE
        simulation_of_sins_core)
    sos_configure_c_target(simulation_of_sins_game_benchmark)
    sos_deploy_runtime(simulation_of_sins_game_benchmark)
endif()

add_executable(simulation_of_sins_physics_benchmark EXCLUDE_FROM_ALL
    physics_spawner_benchmark.c)
target_link_libraries(simulation_of_sins_physics_benchmark PRIVATE
    simulation_of_sins_core laiue::task)
sos_configure_c_target(simulation_of_sins_physics_benchmark)
sos_deploy_runtime(simulation_of_sins_physics_benchmark
    laiue::numeric laiue::world laiue::physics laiue::task)
