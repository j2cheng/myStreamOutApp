
cmake_minimum_required(VERSION 3.10.2)
project("GtestStreamOut")

# Enforce C++17 standard for all targets
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# 1. GStreamer sysroot
#
# The GStreamer distribution is produced once by the shared script
#     platforms/android/scripts/build_gstreamer.sh
# and exposed via the GSTREAMER_SYSROOT env variable (see env.sh).  We
# consume it here to (a) build libgstreamer_android.so via ndk-build and
# (b) resolve GStreamer / GLib include paths.
# --------------------------------------------------------------------------
if(NOT DEFINED GSTREAMER_SYSROOT OR GSTREAMER_SYSROOT STREQUAL "")
    if(DEFINED ENV{GSTREAMER_SYSROOT} AND NOT "$ENV{GSTREAMER_SYSROOT}" STREQUAL "")
        set(GSTREAMER_SYSROOT "$ENV{GSTREAMER_SYSROOT}")
    else()
        message(FATAL_ERROR
            "GSTREAMER_SYSROOT is not set. Source platforms/android/env.sh "
            "or run build_android.sh from the project root.")
    endif()
endif()
message(STATUS "GSTREAMER_SYSROOT = ${GSTREAMER_SYSROOT}")

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "Only arm64-v8a is supported (got '${ANDROID_ABI}'). "
        "The shared GStreamer sysroot is built for android_arm64.")
endif()

# --------------------------------------------------------------------------
# 2. libgstreamer_android.so
#
# Delegate to ndk-build with the app/src/main/jni/{Android,Application}.mk
# recipe.  ndk-build drops the output under app/src/main/libs/<abi>/, which
# app/build.gradle picks up via sourceSets.main.jniLibs.srcDirs so the
# .so is packaged inside the APK.  Nothing else in the workspace shares
# this output directory, so a clean of app/src/main/libs (or ./gradlew
# clean + rm -rf app/.cxx) is sufficient for a clean native rebuild.
# --------------------------------------------------------------------------
find_program(NDK_BUILD ndk-build HINTS $ENV{ANDROID_NDK_HOME} $ENV{ANDROID_NDK_ROOT})
if(NOT NDK_BUILD)
    message(FATAL_ERROR "ndk-build not found; set ANDROID_NDK_HOME.")
endif()

# jni/Android.mk lives under app/src/main/gst/jni; ndk-build's default output
# layout puts libs/, obj/, src/ (generated GStreamer.java) and gst-android-build/
# next to jni/, keeping all ndk-build residue inside app/src/main/gst/.
set(NDK_PROJECT_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/../gst")
set(GSTREAMER_ANDROID_SO "${NDK_PROJECT_DIR}/libs/${ANDROID_ABI}/libgstreamer_android.so")
set(LIBCPP_SHARED_SO     "${NDK_PROJECT_DIR}/libs/${ANDROID_ABI}/libc++_shared.so")

add_custom_command(
    OUTPUT  "${GSTREAMER_ANDROID_SO}" "${LIBCPP_SHARED_SO}"
    COMMAND ${CMAKE_COMMAND} -E env
                "GSTREAMER_SYSROOT=${GSTREAMER_SYSROOT}"
                ${NDK_BUILD} -C ${NDK_PROJECT_DIR}
    COMMENT "ndk-build libgstreamer_android.so (${ANDROID_ABI})"
    VERBATIM
)

add_custom_target(do_ndk_build ALL
    DEPENDS "${GSTREAMER_ANDROID_SO}" "${LIBCPP_SHARED_SO}"
)

add_library(gstreamer_android SHARED IMPORTED)
set_target_properties(gstreamer_android PROPERTIES
    IMPORTED_LOCATION "${GSTREAMER_ANDROID_SO}")
add_dependencies(gstreamer_android do_ndk_build)

# --------------------------------------------------------------------------
# 3. gtest (from NDK third_party)
# --------------------------------------------------------------------------
set(GOOGLETEST_SRC_DIR "${ANDROID_NDK}/sources/third_party/googletest")
add_library(gtest STATIC
    "${GOOGLETEST_SRC_DIR}/src/gtest-all.cc"
    "${GOOGLETEST_SRC_DIR}/src/gtest_main.cc"
)
target_include_directories(gtest
    PUBLIC  "${GOOGLETEST_SRC_DIR}/include"
    PRIVATE "${GOOGLETEST_SRC_DIR}")

# --------------------------------------------------------------------------
# 4. native-lib
# --------------------------------------------------------------------------
add_library(native-lib SHARED
    streamout.cpp
    camera2_gst_streamer.cpp
    jniMock.cpp
    csioMock.cpp
    gst_element_print_properties.cpp
    cam2_streamout_debug.cpp
)
add_dependencies(native-lib do_ndk_build)

target_include_directories(native-lib PRIVATE
    "${GSTREAMER_SYSROOT}/include/gstreamer-1.0"
    "${GSTREAMER_SYSROOT}/lib/gstreamer-1.0/include"
    "${GSTREAMER_SYSROOT}/include/glib-2.0"
    "${GSTREAMER_SYSROOT}/lib/glib-2.0/include"
    "${GOOGLETEST_SRC_DIR}/include"
    include
)

target_link_libraries(native-lib
    gstreamer_android
    gtest
    log
    android
)
