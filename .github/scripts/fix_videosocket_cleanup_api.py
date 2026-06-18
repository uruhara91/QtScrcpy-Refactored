from pathlib import Path

root = Path(__file__).resolve().parents[2]
path = root / "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp"
text = path.read_text(encoding="utf-8-sig")
old = "        m_videoSocket->quit();\n"
new = "        m_videoSocket->quitNotify();\n"
if text.count(old) != 1:
    raise RuntimeError(f"expected one VideoSocket quit call, found {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("VideoSocket cleanup API corrected")
