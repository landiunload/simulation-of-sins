# Runs the interactive client with SOS_STARTUP_PROFILE. The client closes
# itself once the chunk cube is fully processed, and the startup CSV must
# contain both the named stage rows and one consistent first_filled row:
# every queued chunk was built, at least one mesh was uploaded, and at least
# one frame was rendered.
#
# Required: SOS_CLIENT, SOS_CLIENT_DIR, SOS_PROFILE.

if(NOT EXISTS "${SOS_CLIENT}")
    message(FATAL_ERROR "client executable not found: ${SOS_CLIENT}")
endif()

file(REMOVE "${SOS_PROFILE}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            --unset=SOS_FRAME_PROFILE
            "SOS_STARTUP_PROFILE=${SOS_PROFILE}"
            "SOS_NO_VSYNC=1"
            "${SOS_CLIENT}"
    WORKING_DIRECTORY "${SOS_CLIENT_DIR}"
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "client exited with ${_result}")
endif()

file(STRINGS "${SOS_PROFILE}" _rows)
set(_filled "")
set(_stage_rows 0)
foreach(_row IN LISTS _rows)
    if(_row MATCHES "^first_filled,")
        set(_filled "${_row}")
    elseif(_row MATCHES "^[a-z_]+,.*")
        math(EXPR _stage_rows "${_stage_rows} + 1")
    endif()
endforeach()
if(_filled STREQUAL "")
    message(FATAL_ERROR "no first_filled row in ${SOS_PROFILE}")
endif()
if(_stage_rows LESS 8)
    message(FATAL_ERROR "too few startup stage rows: ${_stage_rows}")
endif()

string(REPLACE "," ";" _fields "${_filled}")
list(LENGTH _fields _field_count)
if(_field_count LESS 10)
    message(FATAL_ERROR "unexpected startup row: ${_filled}")
endif()
list(GET _fields 3 _queued)
list(GET _fields 4 _uploaded)
list(GET _fields 5 _builds)
list(GET _fields 6 _peak)
list(GET _fields 8 _frames)
if(_queued LESS 1 OR _uploaded LESS 1 OR _frames LESS 1)
    message(FATAL_ERROR "empty startup row: ${_filled}")
endif()
if(NOT _queued EQUAL _builds)
    message(FATAL_ERROR "queued chunks were not all built: ${_filled}")
endif()
if(_peak LESS _queued)
    message(FATAL_ERROR "peak unfinished work below queued chunks: ${_filled}")
endif()

message(STATUS "startup profile ok: ${_filled}")
