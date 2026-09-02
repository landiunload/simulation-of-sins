include_guard(GLOBAL)

function(sos_release_bundle_strip_command
    out_variable bundle_directory executable_target)
    set(strip_command)
    if(NOT WIN32)
        list(APPEND strip_command
            COMMAND "${CMAKE_COMMAND}"
                "-DBUNDLE_DIRECTORY=${bundle_directory}"
                "-DCONFIGURATION=$<CONFIG>"
                "-DSTRIP_TOOL=${CMAKE_STRIP}"
                "-DAPPLE_PLATFORM=${APPLE}"
                "-DEXECUTABLE_NAME=$<TARGET_FILE_NAME:${executable_target}>"
                -P "${PROJECT_SOURCE_DIR}/cmake/StripReleaseBundle.cmake")
    endif()
    set(${out_variable} "${strip_command}" PARENT_SCOPE)
endfunction()

function(sos_deploy_runtime target_name)
    # An always-run helper target must not drag an EXCLUDE_FROM_ALL target
    # (the manual benchmark) into the ordinary build. Such a target keeps only
    # its POST_BUILD copy, which already runs whenever it is relinked.
    get_target_property(sos_runtime_excluded ${target_name} EXCLUDE_FROM_ALL)
    if(sos_runtime_excluded)
        set(sos_runtime_all_argument)
    else()
        set(sos_runtime_all_argument ALL)
    endif()

    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
            COMMAND_EXPAND_LISTS
            VERBATIM)

        # Imported engine DLLs can change without forcing the executable to
        # relink. Keep an always-run synchronization target so ordinary builds
        # and bundles never retain stale runtime binaries.
        add_custom_target(${target_name}_runtime ${sos_runtime_all_argument}
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
            DEPENDS ${target_name}
            COMMAND_EXPAND_LISTS
            VERBATIM)
    else()
        if(NOT ARGN)
            message(FATAL_ERROR
                "sos_deploy_runtime(${target_name}) requires imported runtime targets")
        endif()
        if(APPLE)
            set(sos_runtime_origin "@loader_path")
        else()
            set(sos_runtime_origin "$ORIGIN")
        endif()
        set_target_properties(${target_name} PROPERTIES
            BUILD_RPATH "${sos_runtime_origin}"
            INSTALL_RPATH "${sos_runtime_origin}")
        set(sos_runtime_files)
        set(sos_stale_static_runtime_files)
        foreach(sos_runtime_target IN LISTS ARGN)
            if(NOT TARGET "${sos_runtime_target}")
                message(FATAL_ERROR
                    "Unknown runtime target: ${sos_runtime_target}")
            endif()
            get_target_property(sos_runtime_aliased_target
                "${sos_runtime_target}" ALIASED_TARGET)
            if(sos_runtime_aliased_target)
                set(sos_runtime_inspected_target
                    "${sos_runtime_aliased_target}")
            else()
                set(sos_runtime_inspected_target "${sos_runtime_target}")
            endif()
            get_target_property(sos_runtime_target_type
                "${sos_runtime_inspected_target}" TYPE)
            if(sos_runtime_target_type STREQUAL "SHARED_LIBRARY" OR
               sos_runtime_target_type STREQUAL "MODULE_LIBRARY")
                list(APPEND sos_runtime_files
                    "$<TARGET_FILE:${sos_runtime_target}>")
            elseif(sos_runtime_target_type STREQUAL "STATIC_LIBRARY")
                list(APPEND sos_stale_static_runtime_files
                    "$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_NAME:${sos_runtime_target}>")
            endif()
        endforeach()

        set(sos_runtime_helper_targets)
        if(sos_runtime_files)
            # Keep the portable executable runnable outside the engine SDK
            # tree. This target is intentionally always-run: an imported
            # engine library can change without forcing the executable to
            # relink. Static archives are link inputs, not runtime files.
            add_custom_target(${target_name}_runtime_copy
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${sos_runtime_files}
                    $<TARGET_FILE_DIR:${target_name}>
                DEPENDS ${target_name}
                COMMAND_EXPAND_LISTS
                VERBATIM)
            list(APPEND sos_runtime_helper_targets
                ${target_name}_runtime_copy)
        endif()
        if(sos_stale_static_runtime_files)
            add_custom_target(${target_name}_runtime_cleanup
                COMMAND "${CMAKE_COMMAND}" -E rm -f
                    ${sos_stale_static_runtime_files}
                DEPENDS ${target_name}
                COMMAND_EXPAND_LISTS
                VERBATIM)
            list(APPEND sos_runtime_helper_targets
                ${target_name}_runtime_cleanup)
        endif()
        add_custom_target(${target_name}_runtime ${sos_runtime_all_argument}
            DEPENDS ${target_name} ${sos_runtime_helper_targets})
    endif()
endfunction()
