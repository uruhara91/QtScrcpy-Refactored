from pathlib import Path

path = Path("QtScrcpy/QtScrcpyCore/src/device/device.cpp")
text = path.read_text(encoding="utf-8-sig")
old = '''void Device::showTouch(bool show)
{
    auto *adb = new AdbProcess();
    connect(adb, &AdbProcess::adbProcessResult, adb,
            [adb](AdbProcess::ADB_EXEC_RESULT result) {
        if (result != AdbProcess::AER_SUCCESS_START) adb->deleteLater();
    });
    adb->setShowTouchesEnabled(getSerial(), show);
}
'''
new = '''void Device::showTouch(bool show)
{
    auto *adb = new AdbProcess(this);
    connect(adb, &AdbProcess::adbProcessResult, adb,
            [adb](AdbProcess::ADB_EXEC_RESULT result) {
        if (result != AdbProcess::AER_SUCCESS_START) adb->deleteLater();
    });
    adb->setShowTouchesEnabled(getSerial(), show);
}
'''
if text.count(old) != 1:
    raise SystemExit("device.cpp: showTouch ownership block mismatch")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
