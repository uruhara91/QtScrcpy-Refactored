#ifndef DEVICEMSG_H
#define DEVICEMSG_H

#include <QByteArray>
#include <QString>

#define DEVICE_MSG_MAX_SIZE (1 << 18) // 256k
// type: 1 byte; length: 4 bytes
#define DEVICE_MSG_TEXT_MAX_LENGTH (DEVICE_MSG_MAX_SIZE - 5)

class DeviceMsg
{
public:
    enum DeviceMsgType
    {
        DMT_NULL = -1,
        DMT_GET_CLIPBOARD = 0,
    };

    DeviceMsg() = default;
    ~DeviceMsg() = default;

    [[nodiscard]] DeviceMsgType type() const noexcept;
    void getClipboardMsgData(QString &text) const;

    // Returns the complete message size, 0 when more bytes are needed, and -1
    // for malformed/unsupported data. State is committed only for a complete
    // message, so fragmented socket reads are safe.
    [[nodiscard]] qint32 deserialize(const QByteArray &byteArray);

private:
    DeviceMsgType m_type = DMT_NULL;
    QByteArray m_clipboardText;
};

#endif // DEVICEMSG_H
