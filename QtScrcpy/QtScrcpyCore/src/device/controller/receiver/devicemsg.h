#ifndef DEVICEMSG_H
#define DEVICEMSG_H

#include <QBuffer>

#define DEVICE_MSG_MAX_SIZE (1 << 18) // 256k
// type: 1 byte; length: 4 bytes
#define DEVICE_MSG_TEXT_MAX_LENGTH (DEVICE_MSG_MAX_SIZE - 5)

class DeviceMsg : public QObject
{
    Q_OBJECT
public:
    enum DeviceMsgType
    {
        DMT_NULL = -1,
        DMT_GET_CLIPBOARD = 0,
    };

    explicit DeviceMsg(QObject *parent = nullptr);
    ~DeviceMsg() override;

    DeviceMsgType type();
    void getClipboardMsgData(QString &text);

    qint32 deserialize(const QByteArray &byteArray);

private:
    struct DeviceMsgData
    {
        DeviceMsgType type = DMT_NULL;
        union
        {
            struct
            {
                char *text = Q_NULLPTR;
            } clipboardMsg;
        };
        DeviceMsgData() {}
        ~DeviceMsgData() {}
    };

    DeviceMsgData m_data;
};

#endif // DEVICEMSG_H
