#include <QDebug>
#include <algorithm>
#include <array>
#include <cstring>

#include "controlmsg.h"

#include "bufferutil.h"

namespace {

using BufferUtil::write16;
using BufferUtil::write32;
using BufferUtil::write64;

inline void writePosition(char *&cursor, const QRect &value) noexcept
{
    write32(cursor, static_cast<quint32>(value.left()));
    write32(cursor, static_cast<quint32>(value.top()));
    write16(cursor, static_cast<quint16>(value.width()));
    write16(cursor, static_cast<quint16>(value.height()));
}

inline quint16 floatToU16fp(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    quint32 converted = static_cast<quint32>(value * 0x1p16f);
    if (converted >= 0xffff) converted = 0xffff;
    return static_cast<quint16>(converted);
}

inline qint16 floatToI16fp(float value) noexcept
{
    value = std::clamp(value, -1.0f, 1.0f);
    qint32 converted = static_cast<qint32>(value * 0x1p15f);
    if (converted >= 0x7fff) converted = 0x7fff;
    return static_cast<qint16>(converted);
}

QByteArray truncateUtf8(const QString &text, int maxBytes)
{
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= maxBytes) return utf8;

    int cut = maxBytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(utf8.at(cut)) & 0xc0u) == 0x80u) {
        --cut;
    }
    utf8.truncate(cut);
    return utf8;
}

} // namespace

ControlMsg::ControlMsg(ControlMsgType controlMsgType)
    : QScrcpyEvent(Control), m_type(controlMsgType)
{
}

void ControlMsg::setInjectKeycodeMsgData(AndroidKeyeventAction action,
                                         AndroidKeycode keycode,
                                         quint32 repeat,
                                         AndroidMetastate metastate)
{
    m_data = InjectKeycodeData{action, keycode, repeat, metastate};
}

void ControlMsg::setInjectTextMsgData(const QString &text)
{
    m_data = InjectTextData{truncateUtf8(text, CONTROL_MSG_INJECT_TEXT_MAX_LENGTH)};
}

void ControlMsg::setInjectTouchMsgData(
    quint64 id,
    AndroidMotioneventAction action,
    AndroidMotioneventButtons actionButtons,
    AndroidMotioneventButtons buttons,
    QRect position,
    float pressure)
{
    m_data = InjectTouchData{id, action, actionButtons, buttons, position, pressure};
}

void ControlMsg::setInjectScrollMsgData(QRect position,
                                        float hScroll,
                                        float vScroll,
                                        AndroidMotioneventButtons buttons)
{
    m_data = InjectScrollData{position, hScroll, vScroll, buttons};
}

void ControlMsg::setGetClipboardMsgData(ControlMsg::GetClipboardCopyKey copyKey)
{
    m_data = GetClipboardData{copyKey};
}

void ControlMsg::setSetClipboardMsgData(const QString &text, bool paste)
{
    m_data = SetClipboardData{
        0, truncateUtf8(text, CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH), paste};
}

void ControlMsg::setDisplayPowerData(bool on)
{
    m_data = SetDisplayPowerData{on};
}

void ControlMsg::setBackOrScreenOnData(bool down)
{
    m_data = BackOrScreenOnData{down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP};
}

int ControlMsg::serializeTo(std::span<char> output) const noexcept
{
    int required = 1;
    switch (m_type) {
    case CMT_INJECT_KEYCODE: required = 14; break;
    case CMT_INJECT_TOUCH: required = 32; break;
    case CMT_INJECT_SCROLL: required = 21; break;
    case CMT_BACK_OR_SCREEN_ON:
    case CMT_GET_CLIPBOARD:
    case CMT_SET_DISPLAY_POWER: required = 2; break;
    case CMT_EXPAND_NOTIFICATION_PANEL:
    case CMT_EXPAND_SETTINGS_PANEL:
    case CMT_COLLAPSE_PANELS:
    case CMT_ROTATE_DEVICE: required = 1; break;
    case CMT_INJECT_TEXT:
    case CMT_SET_CLIPBOARD: return -1;
    default: return 0;
    }

    if (output.size() < static_cast<std::size_t>(required)) return 0;

    char *cursor = output.data();
    *cursor++ = static_cast<char>(m_type);

    switch (m_type) {
    case CMT_INJECT_KEYCODE: {
        const auto &d = std::get<InjectKeycodeData>(m_data);
        *cursor++ = static_cast<char>(d.action);
        write32(cursor, static_cast<quint32>(d.keycode));
        write32(cursor, d.repeat);
        write32(cursor, static_cast<quint32>(d.metastate));
        break;
    }
    case CMT_INJECT_TOUCH: {
        const auto &d = std::get<InjectTouchData>(m_data);
        *cursor++ = static_cast<char>(d.action);
        write64(cursor, d.id);
        writePosition(cursor, d.position);
        write16(cursor, floatToU16fp(d.pressure));
        write32(cursor, static_cast<quint32>(d.actionButtons));
        write32(cursor, static_cast<quint32>(d.buttons));
        break;
    }
    case CMT_INJECT_SCROLL: {
        const auto &d = std::get<InjectScrollData>(m_data);
        writePosition(cursor, d.position);
        const float h = std::clamp(d.hScroll / 16.0f, -1.0f, 1.0f);
        const float v = std::clamp(d.vScroll / 16.0f, -1.0f, 1.0f);
        write16(cursor, static_cast<quint16>(floatToI16fp(h)));
        write16(cursor, static_cast<quint16>(floatToI16fp(v)));
        write32(cursor, static_cast<quint32>(d.buttons));
        break;
    }
    case CMT_BACK_OR_SCREEN_ON:
        *cursor++ = static_cast<char>(std::get<BackOrScreenOnData>(m_data).action);
        break;
    case CMT_GET_CLIPBOARD:
        *cursor++ = static_cast<char>(std::get<GetClipboardData>(m_data).copyKey);
        break;
    case CMT_SET_DISPLAY_POWER:
        *cursor++ = static_cast<char>(std::get<SetDisplayPowerData>(m_data).on);
        break;
    default:
        break;
    }

    return required;
}

QByteArray ControlMsg::serializeData() const
{
    // Reuse one allocation per calling thread. Returning QByteArray by value
    // remains safe: Qt's implicit sharing detaches automatically if a caller
    // keeps an older result alive while a later message is serialized.
    thread_local QByteArray result;

    result.resize(INLINE_SERIALIZED_CAPACITY);
    const int inlineSize = serializeTo(
        std::span<char>(result.data(), static_cast<std::size_t>(result.size())));
    if (inlineSize > 0) {
        result.resize(inlineSize);
        return result;
    }

    if (m_type == CMT_INJECT_TEXT) {
        const auto &d = std::get<InjectTextData>(m_data);
        const int length = d.text.size();
        result.resize(5 + length);
        char *cursor = result.data();
        *cursor++ = static_cast<char>(m_type);
        write32(cursor, static_cast<quint32>(length));
        if (length > 0) {
            std::memcpy(cursor, d.text.constData(), static_cast<std::size_t>(length));
        }
        return result;
    }

    if (m_type == CMT_SET_CLIPBOARD) {
        const auto &d = std::get<SetClipboardData>(m_data);
        const int length = d.text.size();
        result.resize(14 + length);
        char *cursor = result.data();
        *cursor++ = static_cast<char>(m_type);
        write64(cursor, d.sequence);
        *cursor++ = static_cast<char>(d.paste);
        write32(cursor, static_cast<quint32>(length));
        if (length > 0) {
            std::memcpy(cursor, d.text.constData(), static_cast<std::size_t>(length));
        }
        return result;
    }

    result.clear();
    return result;
}
