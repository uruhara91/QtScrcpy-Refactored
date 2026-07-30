#include "devicemsg.h"

#include <limits>

#include "bufferutil.h"

DeviceMsg::DeviceMsgType DeviceMsg::type() const noexcept
{
    return m_type;
}

void DeviceMsg::getClipboardMsgData(QString &text) const
{
    text = QString::fromUtf8(m_clipboardText);
}

qint32 DeviceMsg::deserialize(const QByteArray &byteArray)
{
    constexpr qint32 headerSize = 5;
    if (byteArray.size() < headerSize) return 0;

    const auto candidateType = static_cast<DeviceMsgType>(
        static_cast<unsigned char>(byteArray.at(0)));
    if (candidateType != DMT_GET_CLIPBOARD) return -1;

    const quint32 clipboardLength = BufferUtil::read32(byteArray.constData() + 1);
    if (clipboardLength > DEVICE_MSG_TEXT_MAX_LENGTH) return -1;

    const quint64 totalSize64 = static_cast<quint64>(headerSize) + clipboardLength;
    if (totalSize64 > static_cast<quint64>(std::numeric_limits<qint32>::max())) {
        return -1;
    }

    const qint32 totalSize = static_cast<qint32>(totalSize64);
    if (byteArray.size() < totalSize) return 0;

    // Commit only after the complete payload has arrived.
    m_type = candidateType;
    m_clipboardText = QByteArray(byteArray.constData() + headerSize,
                                 static_cast<int>(clipboardLength));
    return totalSize;
}
