#include <QDebug>

#include "bufferutil.h"
#include "devicemsg.h"

DeviceMsg::DeviceMsg(QObject *parent)
    : QObject(parent)
{
}

DeviceMsg::~DeviceMsg()
{
    if (DMT_GET_CLIPBOARD == m_data.type &&
        Q_NULLPTR != m_data.clipboardMsg.text) {
        delete[] m_data.clipboardMsg.text;
        m_data.clipboardMsg.text = Q_NULLPTR;
    }
}

DeviceMsg::DeviceMsgType DeviceMsg::type()
{
    return m_data.type;
}

void DeviceMsg::getClipboardMsgData(QString &text)
{
    text = QString::fromUtf8(m_data.clipboardMsg.text);
}

qint32 DeviceMsg::deserialize(const QByteArray &byteArray)
{
    QBuffer buffer;
    buffer.setData(byteArray);
    if (!buffer.open(QBuffer::ReadOnly)) return -1;

    const qint64 length = buffer.size();
    if (length < 5) return 0;

    char typeByte = 0;
    if (!buffer.getChar(&typeByte)) return 0;
    m_data.type = static_cast<DeviceMsgType>(typeByte);

    qint32 consumed = 0;
    switch (m_data.type) {
    case DMT_GET_CLIPBOARD: {
        const quint32 clipboardLength = BufferUtil::read32(buffer);
        if (clipboardLength > DEVICE_MSG_TEXT_MAX_LENGTH ||
            clipboardLength > static_cast<quint64>(length - 5)) {
            consumed = 0;
            break;
        }

        const QByteArray text = buffer.read(clipboardLength);
        if (text.size() != static_cast<int>(clipboardLength)) {
            consumed = 0;
            break;
        }

        delete[] m_data.clipboardMsg.text;
        m_data.clipboardMsg.text = new char[text.size() + 1];
        memcpy(m_data.clipboardMsg.text, text.constData(), text.size());
        m_data.clipboardMsg.text[text.size()] = '\0';
        consumed = 5 + static_cast<qint32>(clipboardLength);
        break;
    }
    default:
        qWarning("Unsupported device msg type: %d", static_cast<int>(m_data.type));
        consumed = -1;
        break;
    }

    return consumed;
}
