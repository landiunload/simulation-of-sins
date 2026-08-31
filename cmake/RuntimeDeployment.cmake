include_guard(GLOBAL)

function(sos_deploy_runtime target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target_name}>
            $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS
        VERBATIM)

    # Imported engine DLLs can change without forcing the game executable to
    # relink. Keep an always-run synchronization target so ordinary builds and
    # bundles never retain stale runtime binaries.
    add_custom_target(${target_name}_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target_name}>
            $<TARGET_FILE_DIR:${target_name}>
        DEPENDS ${target_name}
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()
