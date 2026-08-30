include_guard(GLOBAL)

option(SOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SOS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)

if(SOS_ENABLE_CLANG_TIDY)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SOS_ENABLE_CLANG_TIDY requires clang-cl")
    endif()
    find_program(SOS_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_C_CLANG_TIDY
        "${SOS_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
endif()

function(sos_configure_c_target target_name)
    target_compile_features(${target_name} PRIVATE c_std_17)
    target_compile_definitions(${target_name} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE)

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /utf-8
            /fp:strict
            $<$<BOOL:${SOS_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -ffp-contract=off
            $<$<BOOL:${SOS_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endfunction()
