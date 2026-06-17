from pathlib import Path


def read(path: str) -> tuple[Path, str]:
    file_path = Path(path)
    return file_path, file_path.read_text(encoding="utf-8-sig")


def replace_exact(path: str, old: str, new: str, expected: int = 1) -> None:
    file_path, text = read(path)
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"{path}: expected {expected} match(es), found {count}: {old[:120]!r}"
        )
    file_path.write_text(text.replace(old, new), encoding="utf-8")


# 1. Keep shutdown bounded. Normal adb children should exit after SIGKILL, but
# release teardown must never block forever if the OS/process backend is broken.
replace_exact(
    "QtScrcpy/QtScrcpyCore/src/adb/adbprocessimpl.cpp",
    '''    // A QProcess must not be destroyed while the child is still running.
    // Reaching this point should be exceptional after SIGKILL, but waiting for
    // the OS reap is safer than leaking a live adb subprocess or triggering Qt's
    // destruction warning.
    qWarning() << "ADB process did not stop after kill; waiting for reap";
    waitForFinished(-1);
''',
    '''    // Do not turn application shutdown into an unbounded wait. Reaching
    // this path after SIGKILL is exceptional; keep the diagnostic explicit and
    // let Qt report the remaining lifecycle violation rather than hanging.
    qWarning() << "ADB process still running after bounded terminate/kill";
''',
)

# 2. Tunnel cleanup processes are owned by Server. deleteLater() handles normal
# completion; QObject parenting guarantees bounded teardown when the event loop
# is already stopping.
replace_exact(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''bool Server::disableTunnelReverse()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
''',
    '''bool Server::disableTunnelReverse()
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (processResult != qsc::AdbProcess::AER_SUCCESS_START) {
            adb->deleteLater();
        }
    });
    adb->reverseRemove(
        m_params.serial,
        QString(SOCKET_NAME_PREFIX "_%1")
            .arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
''',
)
replace_exact(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''bool Server::disableTunnelForward()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}
''',
    '''bool Server::disableTunnelForward()
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (processResult != qsc::AdbProcess::AER_SUCCESS_START) {
            adb->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}
''',
)

# 3. All telemetry gates and bounded environment reads go through the helper.
decoder_path, decoder = read(
    "QtScrcpy/QtScrcpyCore/src/device/decoder/decoder.cpp"
)
if '#include "qtscrcpytelemetry.h"\n' not in decoder:
    decoder = decoder.replace(
        '#include "compat.h"\n',
        '#include "compat.h"\n#include "qtscrcpytelemetry.h"\n',
        1,
    )
decoder = decoder.replace(
    '    m_telemetryEnabled = qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;\n',
    '    m_telemetryEnabled = qsc::telemetry::enabled();\n',
    1,
)
old_thread_policy = '''int Decoder::selectDecoderThreadCount() const
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
'''
new_thread_policy = '''int Decoder::selectDecoderThreadCount() const
{
    const int logicalCpus = QThread::idealThreadCount();
    const int fallback = logicalCpus > 1 ? logicalCpus - 1 : 1;
    return qsc::telemetry::boundedEnvironmentInt(
        "QTSCRCPY_DECODER_THREADS", fallback, 0, 32);
}
'''
if decoder.count(old_thread_policy) != 1:
    raise SystemExit("decoder.cpp: thread policy block mismatch")
decoder = decoder.replace(old_thread_policy, new_thread_policy, 1)
if decoder.count('    qInfo() << "Decoder queue stats - max depth:"\n') != 2:
    raise SystemExit("decoder.cpp: expected two queue summary log sites")
decoder = decoder.replace(
    '    qInfo() << "Decoder queue stats - max depth:"\n',
    '    qInfo() << "[Telemetry][Decoder] queue maxDepth="\n',
)
decoder = decoder.replace(
    '    logWindow("Decoder queue wait", queue);\n'
    '    logWindow("Decoder worker service", service);\n'
    '    logWindow("Decoded frame interval", interval);\n',
    '    logWindow("[Telemetry][Decoder] queueWait", queue);\n'
    '    logWindow("[Telemetry][Decoder] workerService", service);\n'
    '    logWindow("[Telemetry][Decoder] frameInterval", interval);\n',
    1,
)
if 'qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY")' in decoder:
    raise SystemExit("decoder.cpp: direct telemetry environment read remains")
decoder_path.write_text(decoder, encoding="utf-8")

renderer_path, renderer = read("QtScrcpy/render/qyuvopenglwidget.cpp")
if '#include "qtscrcpytelemetry.h"\n' not in renderer:
    renderer = renderer.replace(
        '#include "qyuvopenglwidget.h"\n',
        '#include "qyuvopenglwidget.h"\n#include "qtscrcpytelemetry.h"\n',
        1,
    )
renderer = renderer.replace(
    '    m_telemetryEnabled = qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;\n',
    '    m_telemetryEnabled = qsc::telemetry::enabled();\n',
    1,
)
renderer = renderer.replace(
    '        qInfo() << "Render mailbox stats - submitted:"\n',
    '        qInfo() << "[Telemetry][Renderer] mailbox submitted="\n',
    1,
)
if 'qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY")' in renderer:
    raise SystemExit("renderer: direct telemetry environment read remains")
renderer_path.write_text(renderer, encoding="utf-8")

# 4. Diagnostics remain opt-in and therefore do not alter normal/package builds.
root_cmake_path, root_cmake = read("QtScrcpy/CMakeLists.txt")
diagnostics_include = (
    '\ninclude("${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtScrcpyDiagnostics.cmake")\n'
)
if "QtScrcpyDiagnostics.cmake" not in root_cmake:
    root_cmake = root_cmake.rstrip() + diagnostics_include
root_cmake_path.write_text(root_cmake, encoding="utf-8")

Path("QtScrcpy/tests/CMakeLists.txt").write_text(
    '''find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Test)

add_executable(QtScrcpyProtocolTests
    protocol_tests.cpp
    ../QtScrcpyCore/src/device/controller/inputconvert/controlmsg.cpp
    ../QtScrcpyCore/src/device/controller/receiver/devicemsg.cpp
)

set_target_properties(QtScrcpyProtocolTests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(QtScrcpyProtocolTests PRIVATE
    Qt${QT_DESIRED_VERSION}::Test
)

target_include_directories(QtScrcpyProtocolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/android
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/inputconvert
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/receiver
)

qtscrcpy_enable_sanitizer(QtScrcpyProtocolTests)
add_test(NAME protocol COMMAND QtScrcpyProtocolTests)
''',
    encoding="utf-8",
)

protocol_path, protocol = read("QtScrcpy/tests/protocol_tests.cpp")
if "QTEST_MAIN(ProtocolTests)" not in protocol:
    raise SystemExit("protocol_tests.cpp: QTEST_MAIN marker missing")
protocol = protocol.replace(
    "QTEST_MAIN(ProtocolTests)",
    "QTEST_APPLESS_MAIN(ProtocolTests)",
    1,
)
protocol_path.write_text(protocol, encoding="utf-8")

print("release hardening transform completed")
