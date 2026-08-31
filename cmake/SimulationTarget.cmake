include_guard(GLOBAL)

option(SOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SOS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(SOS_ENABLE_LTO "Enable interprocedural optimization in Release" ON)
option(SOS_AGGRESSIVE_INLINING "Use MSVC /Ob3 in Release" ON)
set(SOS_CLANG_LTO_MODE "full" CACHE STRING
    "clang-cl Release LTO mode: thin or full")
set_property(CACHE SOS_CLANG_LTO_MODE PROPERTY STRINGS thin full)
if(NOT SOS_CLANG_LTO_MODE MATCHES "^(thin|full)$")
    message(FATAL_ERROR
        "SOS_CLANG_LTO_MODE must be thin or full, got: ${SOS_CLANG_LTO_MODE}")
endif()

# CMake's MSVC Release default spells out /Ob2 after /O2. When the optional
# aggressive profile is selected, remove that directory-scope default so the
# target-scoped /Ob3 does not produce a D9025 override warning.
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND SOS_AGGRESSIVE_INLINING)
    string(REGEX REPLACE
        "(^|[ \t])[-/]Ob2([ \t]|$)"
        " " SOS_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    set(CMAKE_C_FLAGS_RELEASE "${SOS_C_FLAGS_RELEASE}")
endif()

if(SOS_ENABLE_CLANG_TIDY)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SOS_ENABLE_CLANG_TIDY requires clang-cl")
    endif()
    find_program(SOS_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_C_CLANG_TIDY
        "${SOS_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
endif()

if(SOS_ENABLE_LTO)
    if(MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        include(CheckCCompilerFlag)
        string(TOUPPER "${SOS_CLANG_LTO_MODE}" SOS_CLANG_LTO_MODE_UPPER)
        set(SOS_CLANG_LTO_CHECK
            "SOS_CLANG_${SOS_CLANG_LTO_MODE_UPPER}_LTO_SUPPORTED")
        check_c_compiler_flag(
            "-flto=${SOS_CLANG_LTO_MODE}" ${SOS_CLANG_LTO_CHECK})
        if(NOT ${SOS_CLANG_LTO_CHECK})
            message(FATAL_ERROR
                "clang-cl does not support -flto=${SOS_CLANG_LTO_MODE}")
        endif()
    else()
        include(CheckIPOSupported)
        check_ipo_supported(
            RESULT SOS_LTO_SUPPORTED
            OUTPUT SOS_LTO_ERROR
            LANGUAGES C)
        if(NOT SOS_LTO_SUPPORTED)
            message(FATAL_ERROR
                "SOS_ENABLE_LTO requires compiler IPO support: ${SOS_LTO_ERROR}")
        endif()
    endif()
endif()

function(sos_configure_c_target target_name)
    cmake_parse_arguments(PARSE_ARGV 1 TARGET "FAST_FP" "" "")
    if(TARGET_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for ${target_name}: ${TARGET_UNPARSED_ARGUMENTS}")
    endif()

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
            $<$<CONFIG:Release>:/O2 /Ot /Oi /Gw>
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>,$<BOOL:${SOS_AGGRESSIVE_INLINING}>>:/Ob3>
            $<$<BOOL:${TARGET_FAST_FP}>:/fp:fast>
            $<$<NOT:$<BOOL:${TARGET_FAST_FP}>>:/fp:strict>
            $<$<BOOL:${SOS_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release>:-O3>
            $<$<BOOL:${TARGET_FAST_FP}>:-ffast-math>
            $<$<NOT:$<BOOL:${TARGET_FAST_FP}>>:-fno-fast-math -ffp-contract=off>
            $<$<BOOL:${SOS_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()

    if(SOS_ENABLE_LTO)
        if(MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:-flto=${SOS_CLANG_LTO_MODE}>)
        else()
            set_property(TARGET ${target_name}
                PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    endif()

    get_target_property(target_type ${target_name} TYPE)
    if(MSVC AND target_type STREQUAL "EXECUTABLE")
        target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
    endif()
endfunction()
