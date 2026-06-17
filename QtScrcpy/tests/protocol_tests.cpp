#include <QtTest>
#include <array>

#include "controlmsg.h"
#include "devicemsg.h"

namespace {

void appendBigEndian32(QByteArray &buffer, quint32 value)
{
    buffer.append(static_cast<char>(value >> 24));
    buffer.append(static_cast<char>(value >> 16));
    buffer.append(static_cast<char>(value >> 8));
    buffer.append(static_cast<char>(value));
}

QByteArray clipboardMessage(const QByteArray &utf8)
{
    QByteArray message;
    message.reserve(5 + utf8.size());
    message.append(static_cast<char>(DeviceMsg::DMT_GET_CLIPBOARD));
    appendBigEndian32(message, static_cast<quint32>(utf8.size()));
    message.append(utf8);
    return message;
}

} // namespace

class ProtocolTests final : public QObject
{
    Q_OBJECT

private slots:
    void fixedSizeGetClipboardLayout();
    void setClipboardLayoutUsesUtf8ByteLength();
    void setClipboardPreservesEmbeddedNull();
    void clipboardTruncationKeepsValidUtf8();
    void deviceMessageSupportsFragmentedInput();
    void deviceMessageRejectsMalformedHeaders();
};

void ProtocolTests::fixedSizeGetClipboardLayout()
{
    ControlMsg message(ControlMsg::CMT_GET_CLIPBOARD);
    message.setGetClipboardMsgData(ControlMsg::GCCK_COPY);

    std::array<char, ControlMsg::INLINE_SERIALIZED_CAPACITY> buffer{};
    const int size = message.serializeTo(std::span<char>(buffer));

    QCOMPARE(size, 2);
    QCOMPARE(static_cast<unsigned char>(buffer[0]),
             static_cast<unsigned char>(ControlMsg::CMT_GET_CLIPBOARD));
    QCOMPARE(static_cast<unsigned char>(buffer[1]),
             static_cast<unsigned char>(ControlMsg::GCCK_COPY));
}

void ProtocolTests::setClipboardLayoutUsesUtf8ByteLength()
{
    const QString text = QStringLiteral("A😀B");
    const QByteArray utf8 = text.toUtf8();
    QCOMPARE(utf8.size(), 6);

    ControlMsg message(ControlMsg::CMT_SET_CLIPBOARD);
    message.setSetClipboardMsgData(text, true);
    const QByteArray serialized = message.serializeData();

    QCOMPARE(serialized.size(), 14 + utf8.size());
    QCOMPARE(static_cast<unsigned char>(serialized.at(0)),
             static_cast<unsigned char>(ControlMsg::CMT_SET_CLIPBOARD));
    QCOMPARE(serialized.mid(1, 8), QByteArray(8, '\0'));
    QCOMPARE(static_cast<unsigned char>(serialized.at(9)), 1U);
    QCOMPARE(serialized.mid(10, 4), QByteArray::fromHex("00000006"));
    QCOMPARE(serialized.mid(14), utf8);
}

void ProtocolTests::setClipboardPreservesEmbeddedNull()
{
    QString text = QStringLiteral("A");
    text.append(QChar(u'\0'));
    text.append(QStringLiteral("B"));

    ControlMsg message(ControlMsg::CMT_SET_CLIPBOARD);
    message.setSetClipboardMsgData(text, false);
    const QByteArray serialized = message.serializeData();

    QCOMPARE(serialized.mid(10, 4), QByteArray::fromHex("00000003"));
    QCOMPARE(static_cast<unsigned char>(serialized.at(9)), 0U);
    QCOMPARE(serialized.mid(14), QByteArray("A\0B", 3));
}

void ProtocolTests::clipboardTruncationKeepsValidUtf8()
{
    QString text;
    text.reserve(CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH / 2);
    const QString emoji = QStringLiteral("😀");
    const int repeatCount = CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH / 4 + 4;
    for (int i = 0; i < repeatCount; ++i) text.append(emoji);

    ControlMsg message(ControlMsg::CMT_SET_CLIPBOARD);
    message.setSetClipboardMsgData(text, true);
    const QByteArray serialized = message.serializeData();
    const QByteArray payload = serialized.mid(14);

    QVERIFY(payload.size() <= CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH);
    QCOMPARE(QString::fromUtf8(payload).toUtf8(), payload);
    QCOMPARE(payload.size() % 4, 0);
}

void ProtocolTests::deviceMessageSupportsFragmentedInput()
{
    const QString expected = QStringLiteral("A😀B");
    const QByteArray complete = clipboardMessage(expected.toUtf8());

    DeviceMsg message;
    QCOMPARE(message.deserialize(complete.left(4)), 0);
    QCOMPARE(message.type(), DeviceMsg::DMT_NULL);
    QCOMPARE(message.deserialize(complete.chopped(1)), 0);
    QCOMPARE(message.type(), DeviceMsg::DMT_NULL);
    QCOMPARE(message.deserialize(complete), complete.size());
    QCOMPARE(message.type(), DeviceMsg::DMT_GET_CLIPBOARD);

    QString actual;
    message.getClipboardMsgData(actual);
    QCOMPARE(actual, expected);
}

void ProtocolTests::deviceMessageRejectsMalformedHeaders()
{
    DeviceMsg message;

    QByteArray unsupported(5, '\0');
    unsupported[0] = static_cast<char>(0x7f);
    QCOMPARE(message.deserialize(unsupported), -1);
    QCOMPARE(message.type(), DeviceMsg::DMT_NULL);

    QByteArray oversized;
    oversized.append(static_cast<char>(DeviceMsg::DMT_GET_CLIPBOARD));
    appendBigEndian32(oversized, DEVICE_MSG_TEXT_MAX_LENGTH + 1U);
    QCOMPARE(message.deserialize(oversized), -1);
    QCOMPARE(message.type(), DeviceMsg::DMT_NULL);
}

QTEST_APPLESS_MAIN(ProtocolTests)
#include "protocol_tests.moc"
