# Runs the interactive client in render-smoke mode with a frame profile and
# checks the profile row, which reports the resolved physics_threads and
# indexed columns. This proves the default really attaches the task executor
# (threads > 1) on a multi-core machine and that the environment overrides it.
#
# Required: SOS_CLIENT, SOS_CLIENT_DIR, SOS_PROFILE, SOS_EXPECT_THREADS,
# SOS_EXPECT_INDEXED. Optional: SOS_THREADS_ENV, SOS_BROADPHASE_ENV.
# SOS_EXPECT_THREADS=auto means "must be greater than one".

if(NOT EXISTS "${SOS_CLIENT}")
    message(FATAL_ERROR "client executable not found: ${SOS_CLIENT}")
endif()

set(_env_args --unset=SOS_PHYSICS_THREADS --unset=SOS_PHYSICS_BROADPHASE)
if(DEFINED SOS_THREADS_ENV)
    list(APPEND _env_args "SOS_PHYSICS_THREADS=${SOS_THREADS_ENV}")
endif()
if(DEFINED SOS_BROADPHASE_ENV)
    list(APPEND _env_args "SOS_PHYSICS_BROADPHASE=${SOS_BROADPHASE_ENV}")
endif()

file(REMOVE "${SOS_PROFILE}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${_env_args}
            "SOS_FRAME_PROFILE=${SOS_PROFILE}"
            "${SOS_CLIENT}" --render-smoke
    WORKING_DIRECTORY "${SOS_CLIENT_DIR}"
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "client exited with ${_result}")
endif()

file(STRINGS "${SOS_PROFILE}" _rows)
set(_data)
foreach(_row IN LISTS _rows)
    if(NOT _row STREQUAL "")
        list(APPEND _data "${_row}")
    endif()
endforeach()
list(LENGTH _data _row_count)
if(_row_count LESS 2)
    message(FATAL_ERROR "profile has no data rows: ${SOS_PROFILE}")
endif()
math(EXPR _last "${_row_count} - 1")
list(GET _data ${_last} _row)
string(REPLACE "," ";" _fields "${_row}")
list(LENGTH _fields _field_count)
if(_field_count LESS 24)
    message(FATAL_ERROR "unexpected profile columns: ${_row}")
endif()
list(GET _fields 22 _indexed)
list(GET _fields 23 _threads)

if(SOS_EXPECT_THREADS STREQUAL "auto")
    if(NOT _threads GREATER 1)
        message(FATAL_ERROR "expected default executor (threads > 1), got ${_threads}")
    endif()
else()
    if(NOT _threads EQUAL SOS_EXPECT_THREADS)
        message(FATAL_ERROR "expected threads ${SOS_EXPECT_THREADS}, got ${_threads}")
    endif()
endif()
if(NOT _indexed EQUAL SOS_EXPECT_INDEXED)
    message(FATAL_ERROR "expected indexed ${SOS_EXPECT_INDEXED}, got ${_indexed}")
endif()

message(STATUS "physics profile ok: threads=${_threads} indexed=${_indexed}")
