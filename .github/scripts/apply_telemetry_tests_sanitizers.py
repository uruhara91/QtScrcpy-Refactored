from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8-sig")
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"{path}: expected one match, found {count}: {old[:120]!r}"
        )
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


decoder = Path("QtScrcpy/QtScrcpyCore/src/device/decoder/decoder.cpp")
replace_once(
    decoder,
    '#include "compat.h"\n',
    '#include "compat.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    decoder,
    '    m_telemetryEnabled = qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;\n',
    '    m_telemetryEnabled = qsc::telemetry::enabled();\n',
)
replace_once(
    decoder,
    '''int Decoder::selectDecoderThreadCount() const
{
    bool configured = false;
    const int requested = qEnvironmentVariableIntValue(
        "QTSCRCPY_DECODER_THREADS", &configured);

    if (configured) {
        return requested == 0 ? 0 : qBound(1, requested, 32);
    }

    const int logicalCpus = QThread::idealThreadCount();
    return logicalCpus > 1 ? logicalCpus - 1 : 1;
}
''',
    '''int Decoder::selectDecoderThreadCount() const
{
    if (qEnvironmentVariableIsSet("QTSCRCPY_DECODER_THREADS")) {
        return qsc::telemetry::boundedEnvironmentInt(
            "QTSCRCPY_DECODER_THREADS", 0, 0, 32);
    }

    const int logicalCpus = QThread::idealThreadCount();
    return logicalCpus > 1 ? logicalCpus - 1 : 1;
}
''',
)
replace_once(
    decoder,
    '    qInfo() << "Decoder queue stats - max depth:"\n',
    '    qInfo() << "[Telemetry][Decoder] queue maxDepth="\n',
)
replace_once(
    decoder,
    '    qInfo() << "Decoder queue stats - max depth:"\n',
    '    qInfo() << "[Telemetry][Decoder] queue maxDepth="\n',
)
replace_once(
    decoder,
    '    logWindow("Decoder queue wait", queue);\n'
    '    logWindow("Decoder worker service", service);\n'
    '    logWindow("Decoded frame interval", interval);\n',
    '    logWindow("[Telemetry][Decoder] queueWait", queue);\n'
    '    logWindow("[Telemetry][Decoder] workerService", service);\n'
    '    logWindow("[Telemetry][Decoder] frameInterval", interval);\n',
)

renderer = Path("QtScrcpy/render/qyuvopenglwidget.cpp")
replace_once(
    renderer,
    '#include "qyuvopenglwidget.h"\n',
    '#include "qyuvopenglwidget.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    renderer,
    '    m_telemetryEnabled = qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;\n',
    '    m_telemetryEnabled = qsc::telemetry::enabled();\n',
)
replace_once(
    renderer,
    '        qInfo() << "Render mailbox stats - submitted:"\n',
    '        qInfo() << "[Telemetry][Renderer] mailbox submitted="\n',
)

cmake = Path("QtScrcpy/CMakeLists.txt")
text = cmake.read_text(encoding="utf-8-sig")
anchor = '''target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBS}
    QtScrcpyCore
)
'''
if text.count(anchor) != 1:
    raise SystemExit("QtScrcpy/CMakeLists.txt: final target link block mismatch")
append = anchor + '''
# Optional diagnostics. Normal release builds remain unchanged.
set(QTSCRCPY_SANITIZER "none" CACHE STRING
    "Runtime sanitizer: none, address, thread, or undefined")
set_property(CACHE QTSCRCPY_SANITIZER PROPERTY STRINGS
    none address thread undefined)

function(qtscrcpy_enable_sanitizer target)
    if(MSVC OR QTSCRCPY_SANITIZER STREQUAL "none")
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "QTSCRCPY_SANITIZER requires a Clang or GCC-compatible compiler")
    endif()

    if(QTSCRCPY_SANITIZER STREQUAL "address")
        set(_qtscrcpy_sanitizers "address,undefined")
    elseif(QTSCRCPY_SANITIZER STREQUAL "thread")
        set(_qtscrcpy_sanitizers "thread")
    elseif(QTSCRCPY_SANITIZER STREQUAL "undefined")
        set(_qtscrcpy_sanitizers "undefined")
    else()
        message(FATAL_ERROR
            "Unknown QTSCRCPY_SANITIZER=${QTSCRCPY_SANITIZER}")
    endif()

    target_compile_options(${target} PRIVATE
        -O1 -g3
        -fno-lto
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -funwind-tables
        -fasynchronous-unwind-tables
        -fno-sanitize-recover=all
        -fsanitize=${_qtscrcpy_sanitizers}
    )

    get_target_property(_qtscrcpy_target_type ${target} TYPE)
    if(NOT _qtscrcpy_target_type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target} PRIVATE
            -fno-lto
            -fno-sanitize-recover=all
            -fsanitize=${_qtscrcpy_sanitizers}
        )
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_options(${target} PRIVATE -Wl,--icf=none)
        endif()
    endif()
endfunction()

qtscrcpy_enable_sanitizer(${PROJECT_NAME})
qtscrcpy_enable_sanitizer(QtScrcpyCore)

option(QTSCRCPY_BUILD_TESTS "Build QtScrcpy protocol regression tests" OFF)
if(QTSCRCPY_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
'''
cmake.write_text(text.replace(anchor, append, 1), encoding="utf-8")
