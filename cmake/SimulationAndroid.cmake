include_guard(GLOBAL)

# Packages a NativeActivity library into a signed APK with the Android SDK
# build tools. Each tool can be named explicitly; otherwise the newest
# build-tools and platform under ANDROID_HOME (or ANDROID_SDK_ROOT) are used.
# Without a keystore there is no APK target, while the library still builds.
set(SOS_ANDROID_AAPT2 "" CACHE FILEPATH "aapt2 from the Android SDK build tools")
set(SOS_ANDROID_ZIPALIGN "" CACHE FILEPATH "zipalign from the Android SDK build tools")
set(SOS_ANDROID_APKSIGNER "" CACHE FILEPATH "apksigner from the Android SDK build tools")
set(SOS_ANDROID_FRAMEWORK_JAR "" CACHE FILEPATH "android.jar of platforms/android-<level>")
set(SOS_ANDROID_KEYSTORE "" CACHE FILEPATH "Keystore that signs the APK")
set(SOS_ANDROID_KEYSTORE_PASSWORD "android" CACHE STRING "Password of the signing keystore")

function(_sos_android_find_tool variable directory name)
    if(NOT "${${variable}}" STREQUAL "")
        return()
    endif()
    foreach(suffix IN ITEMS "" ".exe" ".bat")
        if(EXISTS "${directory}/${name}${suffix}")
            set(${variable} "${directory}/${name}${suffix}" CACHE FILEPATH
                "${name} from the Android SDK build tools" FORCE)
            return()
        endif()
    endforeach()
endfunction()

# sos_add_android_apk(<library> MANIFEST <file> OUTPUT <apk>) adds the target
# <library>_apk.
function(sos_add_android_apk library)
    cmake_parse_arguments(PARSE_ARGV 1 APK "" "MANIFEST;OUTPUT" "")
    set(sdk "$ENV{ANDROID_HOME}")
    if(sdk STREQUAL "")
        set(sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    if(NOT sdk STREQUAL "")
        file(GLOB build_tools LIST_DIRECTORIES true "${sdk}/build-tools/*")
        list(SORT build_tools COMPARE NATURAL ORDER DESCENDING)
        if(build_tools)
            list(GET build_tools 0 newest_build_tools)
            _sos_android_find_tool(SOS_ANDROID_AAPT2 "${newest_build_tools}" aapt2)
            _sos_android_find_tool(SOS_ANDROID_ZIPALIGN "${newest_build_tools}" zipalign)
            _sos_android_find_tool(SOS_ANDROID_APKSIGNER "${newest_build_tools}" apksigner)
        endif()
        if(SOS_ANDROID_FRAMEWORK_JAR STREQUAL "")
            file(GLOB jars LIST_DIRECTORIES false "${sdk}/platforms/android-*/android.jar")
            list(SORT jars COMPARE NATURAL ORDER DESCENDING)
            if(jars)
                list(GET jars 0 newest_jar)
                set(SOS_ANDROID_FRAMEWORK_JAR "${newest_jar}" CACHE FILEPATH
                    "android.jar of platforms/android-<level>" FORCE)
            endif()
        endif()
    endif()

    # The target SDK is the API level of the framework the APK links against.
    set(target_sdk "")
    if(SOS_ANDROID_FRAMEWORK_JAR MATCHES "android-([0-9]+)[^/\\]*[/\\]android[.]jar$")
        set(target_sdk "${CMAKE_MATCH_1}")
    endif()
    if(NOT SOS_ANDROID_AAPT2 OR NOT SOS_ANDROID_ZIPALIGN OR NOT SOS_ANDROID_APKSIGNER OR
       NOT target_sdk OR NOT SOS_ANDROID_KEYSTORE)
        message(STATUS "Android APK: off; it needs the SDK build tools, a platform "
                       "android.jar and SOS_ANDROID_KEYSTORE")
        return()
    endif()

    find_program(SOS_POWERSHELL NAMES pwsh powershell REQUIRED)
    math(EXPR version_code
        "${PROJECT_VERSION_MAJOR} * 1000000 + ${PROJECT_VERSION_MINOR} * 1000 + ${PROJECT_VERSION_PATCH}")
    add_custom_target(${library}_apk
        COMMAND "${SOS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File
            "${PROJECT_SOURCE_DIR}/src/android/package_apk.ps1"
            -Aapt2 "${SOS_ANDROID_AAPT2}"
            -Zipalign "${SOS_ANDROID_ZIPALIGN}"
            -Apksigner "${SOS_ANDROID_APKSIGNER}"
            -FrameworkJar "${SOS_ANDROID_FRAMEWORK_JAR}"
            -Manifest "${APK_MANIFEST}"
            -NativeLibrary "$<TARGET_FILE:${library}>"
            -Strip "${CMAKE_STRIP}"
            -Abi "${ANDROID_ABI}"
            -Keystore "${SOS_ANDROID_KEYSTORE}"
            -KeystorePassword "${SOS_ANDROID_KEYSTORE_PASSWORD}"
            -Output "${APK_OUTPUT}"
            -MinSdk "${CMAKE_SYSTEM_VERSION}"
            -TargetSdk "${target_sdk}"
            -VersionName "${PROJECT_VERSION}"
            -VersionCode "${version_code}"
        DEPENDS ${library}
        COMMENT "Packaging ${APK_OUTPUT}"
        VERBATIM)
    message(STATUS "Android APK: ${APK_OUTPUT} (target ${library}_apk)")
endfunction()
