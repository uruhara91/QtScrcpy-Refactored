# For VS2019 and Xcode 12+ support.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

#
# Global config
#

# QC Custom config
set(QC_PROJECT_NAME "QtScrcpy")
# Read version numbers from file
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/appversion QC_FILE_VERSION)
set(QC_PROJECT_VERSION ${QC_FILE_VERSION})

# Project declare
project(${QC_PROJECT_NAME} VERSION ${QC_PROJECT_VERSION} LANGUAGES CXX)
message(STATUS "[${PROJECT_NAME}] Project ${PROJECT_NAME} ${PROJECT_VERSION}")

# QC define

# check arch
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(QC_CPU_ARCH x64)
else()
    set(QC_CPU_ARCH x86)
endif()

# MacOS
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # mac default arch arm64
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64)
    endif()

    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(QC_CPU_ARCH arm64)
    endif()
endif()

message(STATUS "[${PROJECT_NAME}] CPU_ARCH:${QC_CPU_ARCH}")

# CMake set
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# default RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
message(STATUS "[${PROJECT_NAME}] BUILD_TYPE:${CMAKE_BUILD_TYPE}")

# Log configuration
option(ENABLE_DETAILED_LOGS "Enable detailed log output with file and line info" OFF)
if(ENABLE_DETAILED_LOGS)
    message(STATUS "[${PROJECT_NAME}] Detailed logs enabled")
else()
    message(STATUS "[${PROJECT_NAME}] Simple logs enabled")
endif()

# Compiler set
message(STATUS "[${PROJECT_NAME}] C++ compiler ID is: ${CMAKE_CXX_COMPILER_ID}")
if (MSVC)
    # FFmpeg cannot be compiled natively by MSVC version < 12.0 (2013)
    if(MSVC_VERSION LESS 1800)
        message(FATAL_ERROR "[${PROJECT_NAME}] ERROR: MSVC version is older than 12.0 (2013).")
    endif()
    
    # Optimization flags
    add_compile_options(/O2 /Ob2 /Oi /Ot /Oy /GL /arch:AVX2 /MP)

    message(STATUS "[${PROJECT_NAME}] Set Warnings as error")
    # warning level 3 and all warnings as errors
    add_compile_options(/W3 /WX /wd4819 /wd4566)

    # avoid warning C4819
    add_compile_options(/utf-8)

    # enable C++ exceptions
    add_compile_options(/GF /Gy /EHsc)

    # ensure we use minimal "windows.h" lib without the crazy min max macros
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    
    # disable SAFESEH
    add_link_options(/SAFESEH:NO)
endif()

if (NOT MSVC)
    message(STATUS "[${PROJECT_NAME}] Set warnings as error")
    # lots of warnings and all warnings as errors
    add_compile_options(-Wall -Wextra -pedantic -Werror)

    # disable some warning
    add_compile_options(-Wno-nested-anon-types -Wno-c++23-extensions -Wno-overloaded-virtual)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    
    # Compiler Flags
    add_compile_options(
        -march=native -O3 -pipe -fno-plt
        -ffunction-sections -fdata-sections
        -flto=full
        -fomit-frame-pointer -fno-unwind-tables
        -fno-asynchronous-unwind-tables -fno-semantic-interposition
        -fno-math-errno -fno-trapping-math
    )

    # Preprocessor Definitions
    add_compile_definitions(NDEBUG QT_NO_DEBUG)

    # Linker Flags (_link)
    add_link_options(
        -fuse-ld=mold
        -Wl,-O3
        -Wl,--gc-sections
        -Wl,--icf=all
        -Wl,--sort-common
        -Wl,--as-needed
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,--hash-style=gnu
        -flto=full
    )
endif()

# Windows built with Clang (MSYS2 CLANG64: llvm + ucrt + libc++), NOT MSVC.
# Mirrors the Linux block's aggressive-optimization philosophy as closely as
# the target allows, but every flag below is re-verified for Windows/PE
# applicability rather than blindly copied from the Linux block:
#   - No -march=native equivalent chase here: left to the person building
#     to add via CMAKE_CXX_FLAGS if they want it - omitted by default since
#     "native" depends on the actual build machine, not something safe to
#     hardcode into a shared CMakeLists.txt.
#   - No -fno-plt / -Wl,-z,relro,now / --hash-style=gnu / ICF via --icf=all:
#     these are ELF/Linux-linker-specific concepts (PLT, RELRO, GNU hash
#     sections) that don't exist in the PE/COFF format Windows uses: passing
#     them here would either be silently ignored or (worse) rejected by
#     lld-link depending on flag, so they're deliberately left out rather
#     than carried over speculatively.
#   - mold IS available for Windows via MSYS2 CLANG64
#     (mingw-w64-clang-x86_64-mold) and is used the same way as on Linux.
#   - LTO (-flto=full) and function/data sections + gc-sections are
#     supported the same way as on Linux (LLVM's LTO and section GC are not
#     ELF-specific), so those are kept.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT MSVC)
    add_compile_options(
        -O3 -pipe
        -ffunction-sections -fdata-sections
        -flto=full
        -fomit-frame-pointer
        -fno-math-errno -fno-trapping-math
    )

    add_compile_definitions(NDEBUG QT_NO_DEBUG NOMINMAX WIN32_LEAN_AND_MEAN)

    add_link_options(
        -Wl,--gc-sections
        -flto=full
    )
endif()

#
# Qt
#

# Find Qt version
if (NOT QT_DESIRED_VERSION)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    message("   >>> Found Qt version: ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}.${QT_VERSION_PATCH}")
    set(QT_DESIRED_VERSION ${QT_VERSION_MAJOR})
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(qt_required_components Widgets Network Multimedia)

if (QT_DESIRED_VERSION MATCHES 6)
    list(APPEND qt_required_components OpenGL)
    list(APPEND qt_required_components OpenGLWidgets)
    list(APPEND qt_required_components Gui)
    list(APPEND qt_required_components GuiPrivate)
else()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND qt_required_components X11Extras )
    endif()
endif()

find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS ${qt_required_components})

set(LINK_LIBS
    Qt${QT_DESIRED_VERSION}::Widgets
    Qt${QT_DESIRED_VERSION}::Network
    Qt${QT_DESIRED_VERSION}::Multimedia
)

if (QT_DESIRED_VERSION MATCHES 6)
    list(APPEND LINK_LIBS Qt${QT_DESIRED_VERSION}::GuiPrivate)
    list(APPEND LINK_LIBS Qt${QT_DESIRED_VERSION}::OpenGL)
    list(APPEND LINK_LIBS Qt${QT_DESIRED_VERSION}::OpenGLWidgets)
else()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND LINK_LIBS Qt${QT_DESIRED_VERSION}::X11Extras)
    endif()
endif()

message(STATUS "[${PROJECT_NAME}] Qt version is: ${QT_DESIRED_VERSION}")

#
# Optional: native Wayland relative-pointer support (Linux + Qt6 only).
#
# This is strictly additive: if Qt6::WaylandClient or the scanner tool isn't
# available (e.g. qt6-wayland-dev not installed), QSC_HAVE_WAYLAND_RELATIVE_POINTER
# is simply never defined and util/mousetap falls back to its existing
# warp-cursor-based confinement (xmousetap/winmousetap/cocoamousetap) at
# runtime, exactly as it did before this feature existed. No hard build
# dependency is introduced.
set(QSC_HAVE_WAYLAND_RELATIVE_POINTER FALSE)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND QT_DESIRED_VERSION MATCHES 6)
    find_package(Qt6 QUIET COMPONENTS WaylandClient)
    find_package(Qt6WaylandScannerTools QUIET)
    # Wayland::Scanner (the wayland-scanner binary, generates the low-level
    # C protocol bindings that qt6_generate_wayland_protocol_client_sources
    # layers its C++ wrapper on top of) ships as a MODULE-mode find script
    # bundled inside Qt6's CMake tree, not a CONFIG package - it must be
    # found separately even when Qt6::WaylandClient itself is present.
    list(APPEND CMAKE_MODULE_PATH "${Qt6_DIR}/3rdparty/extra-cmake-modules/find-modules")
    # The generated low-level bindings are plain C (wayland-*-protocol.c),
    # not C++. This project only declares LANGUAGES CXX at the top, so
    # without enabling C here those .c sources are silently skipped by
    # target_sources() and the final link fails with undefined references
    # to e.g. zwp_relative_pointer_v1_interface (verified with an isolated
    # test build). Scoped to this block only - does not touch the root
    # project() declaration or affect non-Wayland/non-Linux builds.
    enable_language(C)
    find_package(WaylandScanner QUIET MODULE)
    if (TARGET Qt6::WaylandClient AND TARGET Qt6::qtwaylandscanner AND TARGET Wayland::Scanner)
        set(QSC_HAVE_WAYLAND_RELATIVE_POINTER TRUE)
        message(STATUS "[${PROJECT_NAME}] Qt6::WaylandClient found - native "
                        "Wayland relative-pointer mouse-lock enabled")
    else()
        message(STATUS "[${PROJECT_NAME}] Qt6::WaylandClient/scanner NOT "
                        "fully available - falling back to warp-cursor "
                        "mouse-lock on Wayland (install qt6-wayland-dev and "
                        "wayland-protocols to enable the native path)")
    endif()
endif()

#
# Sources
#

# fontawesome
set(QC_FONTAWESOME_SOURCES
    fontawesome/iconhelper.h
    fontawesome/iconhelper.cpp
)
source_group(fontawesome FILES ${QC_FONTAWESOME_SOURCES})

# uibase
set(QC_UIBASE_SOURCES
    uibase/keepratiowidget.h
    uibase/keepratiowidget.cpp
    uibase/magneticwidget.h
    uibase/magneticwidget.cpp
)
source_group(uibase FILES ${QC_UIBASE_SOURCES})

# audio
set(QC_AUDIO_SOURCES
    audio/audiooutput.h
    audio/audiooutput.cpp
)
source_group(audio FILES ${QC_AUDIO_SOURCES})

# ui
set(QC_UI_SOURCES
    ui/toolform.h
    ui/toolform.cpp
    ui/toolform.ui
    ui/videoform.h
    ui/videoform.cpp
    ui/videoform.ui
    ui/dialog.cpp
    ui/dialog.h
    ui/dialog.ui
    render/qyuvopenglwidget.h
    render/qyuvopenglwidget.cpp
)
source_group(ui FILES ${QC_UI_SOURCES})

# group controller
set(QC_GROUP_CONTROLLER
    groupcontroller/groupcontroller.h
    groupcontroller/groupcontroller.cpp
)
source_group(groupcontroller FILES ${QC_GROUP_CONTROLLER})

# util
set(QC_UTIL_SOURCES
    util/config.h
    util/config.cpp
    util/mousetap/mousetap.h
    util/mousetap/mousetap.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(QC_UTIL_SOURCES ${QC_UTIL_SOURCES} 
        util/mousetap/winmousetap.h
        util/mousetap/winmousetap.cpp
        util/winutils.h
        util/winutils.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(QC_UTIL_SOURCES ${QC_UTIL_SOURCES} 
        util/mousetap/xmousetap.h
        util/mousetap/xmousetap.cpp
    )
    if (QSC_HAVE_WAYLAND_RELATIVE_POINTER)
        set(QC_UTIL_SOURCES ${QC_UTIL_SOURCES}
            util/mousetap/waylandmousetap.h
            util/mousetap/waylandmousetap.cpp
        )
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(QC_UTIL_SOURCES ${QC_UTIL_SOURCES} 
        util/mousetap/cocoamousetap.h
        util/mousetap/cocoamousetap.mm
        util/path.h
        util/path.mm
    )
endif()
source_group(util FILES ${QC_UTIL_SOURCES})

# qrc
set(QC_QRC_SOURCES "res/res.qrc")

# main
set(QC_MAIN_SOURCES
    main.cpp
    ${QC_QRC_SOURCES}
)

# plantform file
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Define VERSION macros for .rc file
    add_compile_definitions(
        VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        VERSION_MINOR=${PROJECT_VERSION_MINOR}
        VERSION_PATCH=${PROJECT_VERSION_PATCH}
        VERSION_RC_STR="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
    )
    set(QC_PLANTFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/res/${PROJECT_NAME}.rc"
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Step 1. add icns to source file, for MACOSX_PACKAGE_LOCATION copy
    set(QC_PLANTFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/res/${PROJECT_NAME}.icns"
    )
endif()

# all sources
set(QC_PROJECT_SOURCES
    ${QC_FONTAWESOME_SOURCES}
    ${QC_UIBASE_SOURCES}
    ${QC_UI_SOURCES}
    ${QC_UTIL_SOURCES}
    ${QC_MAIN_SOURCES}
    ${QC_GROUP_CONTROLLER}
    ${QC_PLANTFORM_SOURCES}
    ${QC_AUDIO_SOURCES}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(QC_RUNTIME_TYPE MACOSX_BUNDLE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(QC_RUNTIME_TYPE WIN32)
endif()

add_executable(${PROJECT_NAME} ${QC_RUNTIME_TYPE} ${QC_PROJECT_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    QTSCRCPY_VERSION="${PROJECT_VERSION}"
    QTSCRCPY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

# Log compile definitions
if(ENABLE_DETAILED_LOGS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_DETAILED_LOGS)
endif()

#
# Internal include path (todo: remove this, use absolute path include)
#

target_include_directories(${PROJECT_NAME} PRIVATE fontawesome)
target_include_directories(${PROJECT_NAME} PRIVATE util)
target_include_directories(${PROJECT_NAME} PRIVATE uibase)
target_include_directories(${PROJECT_NAME} PRIVATE ui)
target_include_directories(${PROJECT_NAME} PRIVATE render)

# output dir
get_property(QC_IS_MUTIL_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
message(STATUS "multi config:" QC_IS_MUTIL_CONFIG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../output/${QC_CPU_ARCH}/${CMAKE_BUILD_TYPE}/$<0:>"
)

#
# plantform deps
#

# windows
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    get_target_property(QSC_BIN_OUTPUT_PATH ${PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    set(QSC_DEPLOY_PATH ${QSC_BIN_OUTPUT_PATH})

    target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi)

    if (NOT MSVC)
        # mold on Windows: available via MSYS2 CLANG64
        # (mingw-w64-clang-x86_64-mold). Only applied if actually found, so
        # a plain MinGW64/UCRT64 (GCC) setup without mold installed still
        # builds - falling back to the linker CMake/Clang picks by default
        # (typically lld or ld.bfd) rather than hard-failing.
        find_program(QSC_MOLD_LINKER NAMES mold ld.mold)
        if (QSC_MOLD_LINKER)
            message(STATUS "[${PROJECT_NAME}] mold linker found, using it")
            target_link_options(${PROJECT_NAME} PRIVATE -fuse-ld=mold)
        else()
            message(STATUS "[${PROJECT_NAME}] mold linker not found, using toolchain default")
        endif()
    endif()
endif()

# MacOS
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # qt6 need 10.15 or later
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")

    # copy bundle file
    get_target_property(MACOS_BUNDLE_PATH ${PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    set(MACOS_BUNDLE_PATH ${MACOS_BUNDLE_PATH}/${PROJECT_NAME}.app/Contents)

    set(QSC_DEPLOY_PATH ${MACOS_BUNDLE_PATH})

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        # config file copy to Contents/MacOS/config
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../config/config.ini" "${MACOS_BUNDLE_PATH}/MacOS/config/config.ini"
    )

    # Step 2. ues MACOSX_PACKAGE_LOCATION copy icns to Resources
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/res/${PROJECT_NAME}.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )

    # use MACOSX_BUNDLE_INFO_PLIST custom plist
    set(INFO_PLIST_TEMPLATE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/Info_Mac.plist.in")
    set(INFO_PLIST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/Info_Mac.plist")
    file(READ "${INFO_PLIST_TEMPLATE_FILE}" plist_contents)
    string(REPLACE "\${BUNDLE_VERSION}" "${PROJECT_VERSION}" plist_contents ${plist_contents})
    file(WRITE ${INFO_PLIST_FILE} ${plist_contents})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${INFO_PLIST_FILE}"
        # "" disable code sign
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    )

    # mac framework
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework AppKit")
endif()

# Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_target_property(QSC_BIN_OUTPUT_PATH ${PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    set(QSC_DEPLOY_PATH ${QSC_BIN_OUTPUT_PATH})

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/QtScrcpyCore/src/third_party/scrcpy-server" "${QSC_BIN_OUTPUT_PATH}"
    )

    find_package(Threads REQUIRED)

    target_link_libraries(${PROJECT_NAME} PRIVATE
        xcb
        Threads::Threads
    )

    if (QSC_HAVE_WAYLAND_RELATIVE_POINTER)
        # Generates qwayland-relative-pointer-unstable-v1.{h,cpp} and
        # qwayland-pointer-constraints-unstable-v1.{h,cpp} (Qt-style C++
        # wrapper classes, e.g. QtWayland::zwp_relative_pointer_v1) plus the
        # matching low-level wayland-*-client-protocol.{h,c} bindings,
        # straight into the build directory - nothing checked-in/generated
        # source is added to the repo.
        qt6_generate_wayland_protocol_client_sources(${PROJECT_NAME} FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/relative-pointer-unstable-v1.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/pointer-constraints-unstable-v1.xml"
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::WaylandClient)
        target_compile_definitions(${PROJECT_NAME} PRIVATE QSC_HAVE_WAYLAND_RELATIVE_POINTER)
    endif()
endif()

add_subdirectory(QtScrcpyCore)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBS}
    QtScrcpyCore
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtScrcpyDiagnostics.cmake")
