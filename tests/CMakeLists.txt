function(sos_add_unit_test name)
    add_executable(sos_${name}_test ${name}_test.c)
    target_link_libraries(sos_${name}_test PRIVATE ${ARGN})
    sos_configure_c_target(sos_${name}_test)
    add_test(NAME simulation_of_sins.${name} COMMAND sos_${name}_test)
    set_tests_properties(simulation_of_sins.${name} PROPERTIES TIMEOUT 30)
endfunction()

sos_add_unit_test(clock sos_core)
sos_add_unit_test(random sos_core)
sos_add_unit_test(fixed sos_core)
sos_add_unit_test(table sos_core)
sos_add_unit_test(terrain sos_core)
sos_add_unit_test(plants sos_core)
sos_add_unit_test(world sos_core)
sos_add_unit_test(biology sos_core)
sos_add_unit_test(body sos_core)
sos_add_unit_test(bonds sos_core)
sos_add_unit_test(cells sos_core)
sos_add_unit_test(census sos_core)
sos_add_unit_test(chronicle sos_core)
sos_add_unit_test(climate sos_core)
sos_add_unit_test(culture sos_core)
sos_add_unit_test(deposits sos_core)
sos_add_unit_test(disease sos_core)
sos_add_unit_test(events sos_core)
sos_add_unit_test(fauna sos_core)
sos_add_unit_test(genome sos_core)
sos_add_unit_test(groups sos_core)
sos_add_unit_test(items sos_core)
sos_add_unit_test(lifecycle sos_core)
sos_add_unit_test(lineage sos_core)
sos_add_unit_test(links sos_core)
sos_add_unit_test(memories sos_core)
sos_add_unit_test(mood sos_core)
sos_add_unit_test(motives sos_core)
sos_add_unit_test(names sos_core)
sos_add_unit_test(noise sos_core)
sos_add_unit_test(personality sos_core)
sos_add_unit_test(relations sos_core)
sos_add_unit_test(rumors sos_core)
sos_add_unit_test(sins sos_core)
sos_add_unit_test(skills sos_core)
sos_add_unit_test(spatial sos_core)
sos_add_unit_test(status sos_core)
sos_add_unit_test(tech sos_core)
sos_add_unit_test(territory sos_core)
sos_add_unit_test(water sos_core)
sos_add_unit_test(arguments sos_app)
if(TARGET sos_client)
    sos_add_unit_test(touch sos_client)
endif()

if(SOS_BUILD_HEADLESS)
    # Real-time loop, accelerated: 300 ticks at a thousandfold time scale.
    add_test(NAME simulation_of_sins.headless
        COMMAND simulation_of_sins_headless --ticks 300 --time-scale 1000)
    add_test(NAME simulation_of_sins.headless_paced
        COMMAND simulation_of_sins_headless --ticks 15)
    add_test(NAME simulation_of_sins.headless_rejects_options
        COMMAND simulation_of_sins_headless --ticks -1)
    set_tests_properties(simulation_of_sins.headless_rejects_options PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(simulation_of_sins.headless simulation_of_sins.headless_paced
        simulation_of_sins.headless_rejects_options PROPERTIES TIMEOUT 30)
endif()

if(SOS_BUILD_CLIENT)
    # Presents three frames through the engine modules copied next to the
    # client; fails when no window and graphics provider can present.
    add_test(NAME simulation_of_sins.client_smoke COMMAND simulation_of_sins --smoke)
    set_tests_properties(simulation_of_sins.client_smoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:simulation_of_sins>"
        RESOURCE_LOCK gpu
        TIMEOUT 60)

    # Every engine module is optional. Each case stages the client with some
    # modules removed and checks that only the matching technology goes away.
    set(sos_window "$<TARGET_FILE_NAME:laiue::window>")
    set(sos_input "$<TARGET_FILE_NAME:laiue::input>")
    set(sos_graphics)
    foreach(provider IN ITEMS laiue::graphics_d3d12 laiue::graphics_vulkan)
        if(TARGET ${provider})
            list(APPEND sos_graphics "$<TARGET_FILE_NAME:${provider}>")
        endif()
    endforeach()
    set(sos_no_presentation 4)
    function(sos_add_client_modules_test name expected_exit expected_output)
        # A list cannot cross the test command line intact; '|' separates it.
        list(JOIN ARGN "|" modules)
        add_test(NAME simulation_of_sins.client_modules.${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DCLIENT=$<TARGET_FILE:simulation_of_sins>"
                "-DMODULE_DIRECTORY=$<TARGET_FILE_DIR:simulation_of_sins>"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/client_modules/${name}"
                "-DMODULES=${modules}"
                "-DEXPECT_EXIT=${expected_exit}"
                "-DEXPECT_OUTPUT=${expected_output}"
                "-DARGUMENTS=--smoke"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/client_modules_test.cmake")
        set_tests_properties(simulation_of_sins.client_modules.${name} PROPERTIES
            RESOURCE_LOCK gpu
            TIMEOUT 60)
    endfunction()
    sos_add_client_modules_test(none ${sos_no_presentation} "no window and graphics provider")
    sos_add_client_modules_test(without_window ${sos_no_presentation}
        "engine module window is off" ${sos_input} ${sos_graphics})
    sos_add_client_modules_test(without_graphics ${sos_no_presentation}
        "no window and graphics provider" ${sos_window} ${sos_input})
    set(sos_math "$<TARGET_FILE_NAME:laiue::scene_math>")
    set(sos_mesher "$<TARGET_FILE_NAME:laiue::mesher>")
    sos_add_client_modules_test(without_input 0 "input is unavailable.*3 of 3 frames"
        ${sos_window} ${sos_graphics} ${sos_math} ${sos_mesher})
    sos_add_client_modules_test(without_camera_math 0
        "camera math is unavailable.*3 of 3 frames"
        ${sos_window} ${sos_input} ${sos_graphics} ${sos_mesher})
    sos_add_client_modules_test(without_mesher 0 "surface meshing is unavailable.*3 of 3 frames"
        ${sos_window} ${sos_input} ${sos_graphics} ${sos_math})
    if(TARGET laiue::graphics_d3d12 AND TARGET laiue::graphics_vulkan)
        # Without the native provider the client must move on to Vulkan. Whether
        # Vulkan presents depends on the machine's driver; either way the client
        # reports the outcome instead of crashing.
        sos_add_client_modules_test(without_d3d12 "0|${sos_no_presentation}"
            "trying laiue.graphics.vulkan" ${sos_window} ${sos_input}
            "$<TARGET_FILE_NAME:laiue::graphics_vulkan>")
    endif()
endif()

add_test(NAME simulation_of_sins.engine_revision
    COMMAND "${CMAKE_COMMAND}"
        "-DSOS_REVISION_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/engine_revision"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_revision_test.cmake")
set_tests_properties(simulation_of_sins.engine_revision PROPERTIES TIMEOUT 30)
