add_executable(simulation_of_sins_game_test game_test.c)
target_link_libraries(simulation_of_sins_game_test PRIVATE
    simulation_of_sins_core)
if(NOT WIN32)
    target_link_libraries(simulation_of_sins_game_test PRIVATE m)
endif()

sos_configure_c_target(simulation_of_sins_game_test)
sos_deploy_runtime(simulation_of_sins_game_test laiue::numeric laiue::world laiue::physics)

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

    # The profile row is the only observable that proves the default actually
    # created the physics executor; the environment must still override both
    # the worker count and the broadphase. The default test only makes sense
    # when the machine has more than one logical processor.
    include(ProcessorCount)
    ProcessorCount(SOS_LOGICAL_PROCESSORS)
    add_test(NAME simulation_of_sins.physics_overrides
        COMMAND "${CMAKE_COMMAND}"
            "-DSOS_CLIENT=$<TARGET_FILE:simulation_of_sins>"
            "-DSOS_CLIENT_DIR=$<TARGET_FILE_DIR:simulation_of_sins>"
            "-DSOS_PROFILE=${CMAKE_CURRENT_BINARY_DIR}/physics_overrides.csv"
            "-DSOS_EXPECT_THREADS=1"
            "-DSOS_EXPECT_INDEXED=1"
            "-DSOS_THREADS_ENV=1"
            "-DSOS_BROADPHASE_ENV=tree"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/physics_defaults_test.cmake")
    set_tests_properties(simulation_of_sins.physics_overrides
        PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
            RESOURCE_LOCK d3d12
            TIMEOUT 30)
    if(SOS_LOGICAL_PROCESSORS GREATER 1)
        add_test(NAME simulation_of_sins.physics_defaults
            COMMAND "${CMAKE_COMMAND}"
                "-DSOS_CLIENT=$<TARGET_FILE:simulation_of_sins>"
                "-DSOS_CLIENT_DIR=$<TARGET_FILE_DIR:simulation_of_sins>"
                "-DSOS_PROFILE=${CMAKE_CURRENT_BINARY_DIR}/physics_defaults.csv"
                "-DSOS_EXPECT_THREADS=auto"
                "-DSOS_EXPECT_INDEXED=0"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/physics_defaults_test.cmake")
        set_tests_properties(simulation_of_sins.physics_defaults
            PROPERTIES
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
                RESOURCE_LOCK d3d12
                TIMEOUT 30)
    endif()

    # Сквозная проверка измерительной обвязки запуска: клиент сам закрывается
    # на первом полностью обработанном кубе чанков, а CSV обязан содержать
    # стадии и согласованный first_filled (все заявки построены, меши загружены).
    add_test(NAME simulation_of_sins.startup_profile
        COMMAND "${CMAKE_COMMAND}"
            "-DSOS_CLIENT=$<TARGET_FILE:simulation_of_sins>"
            "-DSOS_CLIENT_DIR=$<TARGET_FILE_DIR:simulation_of_sins>"
            "-DSOS_PROFILE=${CMAKE_CURRENT_BINARY_DIR}/startup_profile.csv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/startup_profile_test.cmake")
    set_tests_properties(simulation_of_sins.startup_profile
        PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
            RESOURCE_LOCK d3d12
            TIMEOUT 30)

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

add_test(NAME simulation_of_sins.engine_revision
    COMMAND "${CMAKE_COMMAND}"
        "-DSOS_REVISION_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/engine_revision"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_revision_test.cmake")
set_tests_properties(simulation_of_sins.engine_revision PROPERTIES TIMEOUT 30)
