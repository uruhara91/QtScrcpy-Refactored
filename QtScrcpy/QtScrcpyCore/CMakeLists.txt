set(QSC_PROJECT_NAME "QtScrcpyCore")

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- STANDARD SETUP ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt version
if (NOT QT_DESIRED_VERSION)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    message("   >>> Found Qt version: ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}.${QT_VERSION_PATCH}")
    set(QT_DESIRED_VERSION ${QT_VERSION_MAJOR})
endif()

find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Widgets Network)

set(LINK_LIBS
    Qt${QT_DESIRED_VERSION}::Widgets
    Qt${QT_DESIRED_VERSION}::Network
)

# check arch
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(QSC_CPU_ARCH x64)
else()
    set(QSC_CPU_ARCH x86)
endif()

# MacOS
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64)
    endif()

    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(QSC_CPU_ARCH arm64)
    endif()
endif()

message(STATUS "[${PROJECT_NAME}] QSC_CPU_ARCH:${QSC_CPU_ARCH}")

if(NOT DEFINED QSC_DEPLOY_PATH)
    message(FATAL_ERROR "-- [QtScrcpyCore] need QSC_DEPLOY_PATH")
endif()

message("-- [QtScrcpyCore] QSC_DEPLOY_PATH: ${QSC_DEPLOY_PATH}")

# Sources
set(QSC_ADB_SOURCES
    src/adb/adbprocessimpl.h
    src/adb/adbprocessimpl.cpp
    src/adb/adbprocess.cpp
)
source_group(src/adb FILES ${QSC_ADB_SOURCES})

set(QSC_COMMON_SOURCES
    src/common/qscrcpyevent.h
)
source_group(src/common FILES ${QSC_COMMON_SOURCES})

set(QSC_INCLUDE_SOURCES
    include/QtScrcpyCore.h
    include/QtScrcpyCoreDef.h
    include/adbprocess.h
)
source_group(include FILES ${QSC_INCLUDE_SOURCES})

set(QSC_DEVICE_SOURCES
    src/device/device.h
    src/device/device.cpp
    src/device/compat.h
    src/device/android/input.h
    src/device/android/keycodes.h
    src/device/controller/controller.h
    src/device/controller/controller.cpp
    src/device/controller/bufferutil.h
    src/device/controller/bufferutil.cpp
    src/device/controller/inputconvert/inputconvertbase.h
    src/device/controller/inputconvert/inputconvertbase.cpp
    src/device/controller/inputconvert/inputconvertnormal.h
    src/device/controller/inputconvert/inputconvertnormal.cpp
    src/device/controller/inputconvert/inputconvertgame.h
    src/device/controller/inputconvert/inputconvertgame.cpp
    src/device/controller/inputconvert/controlmsg.h
    src/device/controller/inputconvert/controlmsg.cpp
    src/device/controller/inputconvert/keymap/keymap.h
    src/device/controller/inputconvert/keymap/keymap.cpp
    src/device/controller/receiver/devicemsg.h
    src/device/controller/receiver/devicemsg.cpp
    src/device/controller/receiver/receiver.h
    src/device/controller/receiver/receiver.cpp
    src/device/decoder/avframeconvert.h
    src/device/decoder/avframeconvert.cpp
    src/device/decoder/decoder.h
    src/device/decoder/decoder.cpp
    src/device/decoder/fpscounter.h
    src/device/decoder/fpscounter.cpp
    src/device/decoder/videobuffer.h
    src/device/decoder/videobuffer.cpp
    src/device/filehandler/filehandler.h
    src/device/filehandler/filehandler.cpp
    src/device/recorder/recorder.h
    src/device/recorder/recorder.cpp
    src/device/server/server.h
    src/device/server/server.cpp
    src/device/server/tcpserver.h
    src/device/server/tcpserver.cpp
    src/device/server/videosocket.h
    src/device/server/videosocket.cpp
    src/device/demuxer/demuxer.h
    src/device/demuxer/demuxer.cpp
)
source_group(src/device FILES ${QSC_DEVICE_SOURCES})

set(QSC_DEVICEMANAGE_SOURCES
    src/devicemanage/devicemanage.h
    src/devicemanage/devicemanage.cpp
)
source_group(src/devicemanage FILES ${QSC_DEVICEMANAGE_SOURCES})

add_library(QtScrcpyCore STATIC
  ${QSC_ADB_SOURCES}
  ${QSC_COMMON_SOURCES}
  ${QSC_INCLUDE_SOURCES}
  ${QSC_DEVICE_SOURCES}
  ${QSC_DEVICEMANAGE_SOURCES}
)

target_link_libraries(QtScrcpyCore PRIVATE
    ${LINK_LIBS}
)

set_target_properties(${QSC_PROJECT_NAME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${QSC_DEPLOY_PATH}/$<0:>"
)

target_include_directories(${QSC_PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/adb)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/common)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/filehandler)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/android)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/decoder)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/controller)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/controller/receiver)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/controller/inputconvert)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/controller/inputconvert/keymap)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/server)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/demuxer)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/ui)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/device/recorder)
target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/devicemanage)

# --- PLATFORM SPECIFIC ---
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # NOTE: uses lib/windows/ (not lib/ directly) so the committed Windows
    # .a files don't collide by filename with the committed Linux .a files,
    # which both sit under the same third_party/ffmpeg tree in the repo.
    set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg")
    set(FFMPEG_WIN_LIB_DIR "${FFMPEG_DIR}/lib/windows")
    target_include_directories(${QSC_PROJECT_NAME} PUBLIC ${FFMPEG_DIR}/include)
    target_link_libraries(QtScrcpyCore PUBLIC
        ${FFMPEG_WIN_LIB_DIR}/libavcodec.a
        ${FFMPEG_WIN_LIB_DIR}/libavformat.a
        ${FFMPEG_WIN_LIB_DIR}/libavutil.a
        ${FFMPEG_WIN_LIB_DIR}/libswscale.a
        ${FFMPEG_WIN_LIB_DIR}/libswresample.a
        secur32 bcrypt mfplat mfuuid ole32 strmiids
        mimalloc
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/lib/${QSC_CPU_ARCH}")
    target_include_directories(${QSC_PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/include
    )
    target_link_libraries(QtScrcpyCore PUBLIC
        ${FFMPEG_LIB_DIR}/libavcodec.a
        ${FFMPEG_LIB_DIR}/libavformat.a
        ${FFMPEG_LIB_DIR}/libavutil.a
        ${FFMPEG_LIB_DIR}/libswscale.a
        ${FFMPEG_LIB_DIR}/libavdevice.a
        ${FFMPEG_LIB_DIR}/libavfilter.a
        ${FFMPEG_LIB_DIR}/libswresample.a
        "-framework VideoToolbox"
        "-framework CoreVideo"
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Security"
        iconv
        z
        bz2
    )

    set(QSC_DEPLOY_PATH "${CMAKE_BINARY_DIR}/bin/${CMAKE_CFG_INTDIR}/${QC_PROJECT_NAME}.app/Contents/MacOS")
    add_custom_command(TARGET ${QSC_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${QSC_DEPLOY_PATH}/MacOS"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/adb/mac/adb" "${QSC_DEPLOY_PATH}/MacOS"
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # NOTE: uses lib/linux/ (not lib/ directly) so the committed Linux .a
    # files don't collide by filename with the committed Windows .a files,
    # which both sit under the same third_party/ffmpeg tree in the repo.
    set(FFMPEG_STATIC_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/lib/linux")

    target_include_directories(${QSC_PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/include
    )

    target_link_libraries(${QSC_PROJECT_NAME} PUBLIC
        ${FFMPEG_STATIC_LIB_DIR}/libavformat.a
        ${FFMPEG_STATIC_LIB_DIR}/libavcodec.a
        ${FFMPEG_STATIC_LIB_DIR}/libavutil.a
        ${FFMPEG_STATIC_LIB_DIR}/libswscale.a
        ${FFMPEG_STATIC_LIB_DIR}/libswresample.a
        pthread
        z
    )

    get_target_property(QSC_BIN_OUTPUT_PATH ${QSC_PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT QSC_BIN_OUTPUT_PATH)
        set(QSC_BIN_OUTPUT_PATH ${CMAKE_BINARY_DIR})
    endif()

    add_custom_command(TARGET ${QSC_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/scrcpy-server" "${QSC_BIN_OUTPUT_PATH}"
    )
endif()

target_link_libraries(${QSC_PROJECT_NAME} PUBLIC ${LINK_LIBS})
