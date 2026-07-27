#include <QDebug>
#include <algorithm>
#include <array>
#include <cstring>

#include "controlmsg.h"

namespace {

inline void write16(char *&cursor, quint16 value) noexcept
{
    *cursor++ = static_cast<char>(value >> 8);
    *cursor++ = static_cast<char>(value);
}

inline void write32(char *&cursor, quint32 value) noexcept
{
    *cursor++ = static_cast<char>(value >> 24);
    *cursor++ = static_cast<char>(value >> 16);
    *cursor++ = static_cast<char>(value >> 8);
    *cursor++ = static_cast<char>(value);
}

inline void write64(char *&cursor, quint64 value) noexcept
{
    write32(cursor, static_cast<quint32>(value >> 32));
    write32(cursor, static_cast<quint32>(value));
}

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
    : QScrcpyEvent(Control)
{
    m_data.type = controlMsgType;
    if (controlMsgType == CMT_INJECT_TEXT) {
        m_data.injectText.text = nullptr;
        m_data.injectText.length = 0;
    } else if (controlMsgType == CMT_SET_CLIPBOARD) {
        m_data.setClipboard.sequence = 0;
        m_data.setClipboard.text = nullptr;
        m_data.setClipboard.length = 0;
        m_data.setClipboard.paste = true;
    }
}

ControlMsg::~ControlMsg()
{
    if (CMT_SET_CLIPBOARD == m_data.type && m_data.setClipboard.text) {
        delete[] m_data.setClipboard.text;
        m_data.setClipboard.text = nullptr;
    } else if (CMT_INJECT_TEXT == m_data.type && m_data.injectText.text) {
        delete[] m_data.injectText.text;
        m_data.injectText.text = nullptr;
    }
}

void ControlMsg::setInjectKeycodeMsgData(AndroidKeyeventAction action,
                                         AndroidKeycode keycode,
                                         quint32 repeat,
                                         AndroidMetastate metastate)
{
    m_data.injectKeycode.action = action;
    m_data.injectKeycode.keycode = keycode;
    m_data.injectKeycode.repeat = repeat;
    m_data.injectKeycode.metastate = metastate;
}

void ControlMsg::setInjectTextMsgData(const QString &text)
{
    delete[] m_data.injectText.text;
    m_data.injectText.text = nullptr;
    m_data.injectText.length = 0;

    const QByteArray utf8 = truncateUtf8(text, CONTROL_MSG_INJECT_TEXT_MAX_LENGTH);
    if (utf8.isEmpty()) return;

    m_data.injectText.text = new char[utf8.size()];
    m_data.injectText.length = static_cast<quint32>(utf8.size());
    std::memcpy(m_data.injectText.text, utf8.constData(),
                static_cast<std::size_t>(utf8.size()));
}

void ControlMsg::setInjectTouchMsgData(
    quint64 id,
    AndroidMotioneventAction action,
    AndroidMotioneventButtons actionButtons,
    AndroidMotioneventButtons buttons,
    QRect position,
    float pressure)
{
    m_data.injectTouch.id = id;
    m_data.injectTouch.action = action;
    m_data.injectTouch.actionButtons = actionButtons;
    m_data.injectTouch.buttons = buttons;
    m_data.injectTouch.position = position;
    m_data.injectTouch.pressure = pressure;
}

void ControlMsg::setInjectScrollMsgData(QRect position,
                                        float hScroll,
                                        float vScroll,
                                        AndroidMotioneventButtons buttons)
{
    m_data.injectScroll.position = position;
    m_data.injectScroll.hScroll = hScroll;
    m_data.injectScroll.vScroll = vScroll;
    m_data.injectScroll.buttons = buttons;
}

void ControlMsg::setGetClipboardMsgData(ControlMsg::GetClipboardCopyKey copyKey)
{
    m_data.getClipboard.copyKey = copyKey;
}

void ControlMsg::setSetClipboardMsgData(const QString &text, bool paste)
{
    delete[] m_data.setClipboard.text;
    m_data.setClipboard.text = nullptr;
    m_data.setClipboard.length = 0;
    m_data.setClipboard.paste = paste;
    m_data.setClipboard.sequence = 0;

    const QByteArray utf8 = truncateUtf8(text, CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH);
    if (utf8.isEmpty()) return;

    m_data.setClipboard.text = new char[utf8.size()];
    m_data.setClipboard.length = static_cast<quint32>(utf8.size());
    std::memcpy(m_data.setClipboard.text, utf8.constData(),
                static_cast<std::size_t>(utf8.size()));
}

void ControlMsg::setDisplayPowerData(bool on)
{
    m_data.setDisplayPower.on = on;
}

void ControlMsg::setBackOrScreenOnData(bool down)
{
    m_data.backOrScreenOn.action = down ? AKEY_EVENT_ACTION_DOWN
                                        : AKEY_EVENT_ACTION_UP;
}

int ControlMsg::serializeTo(std::span<char> output) const noexcept
{
    int required = 1;
    switch (m_data.type) {
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
    *cursor++ = static_cast<char>(m_data.type);

    switch (m_data.type) {
    case CMT_INJECT_KEYCODE:
        *cursor++ = static_cast<char>(m_data.injectKeycode.action);
        write32(cursor, static_cast<quint32>(m_data.injectKeycode.keycode));
        write32(cursor, m_data.injectKeycode.repeat);
        write32(cursor, static_cast<quint32>(m_data.injectKeycode.metastate));
        break;
    case CMT_INJECT_TOUCH:
        *cursor++ = static_cast<char>(m_data.injectTouch.action);
        write64(cursor, m_data.injectTouch.id);
        writePosition(cursor, m_data.injectTouch.position);
        write16(cursor, floatToU16fp(m_data.injectTouch.pressure));
        write32(cursor, static_cast<quint32>(m_data.injectTouch.actionButtons));
        write32(cursor, static_cast<quint32>(m_data.injectTouch.buttons));
        break;
    case CMT_INJECT_SCROLL: {
        writePosition(cursor, m_data.injectScroll.position);
        const float h = std::clamp(m_data.injectScroll.hScroll / 16.0f, -1.0f, 1.0f);
        const float v = std::clamp(m_data.injectScroll.vScroll / 16.0f, -1.0f, 1.0f);
        write16(cursor, static_cast<quint16>(floatToI16fp(h)));
        write16(cursor, static_cast<quint16>(floatToI16fp(v)));
        write32(cursor, static_cast<quint32>(m_data.injectScroll.buttons));
        break;
    }
    case CMT_BACK_OR_SCREEN_ON:
        *cursor++ = static_cast<char>(m_data.backOrScreenOn.action);
        break;
    case CMT_GET_CLIPBOARD:
        *cursor++ = static_cast<char>(m_data.getClipboard.copyKey);
        break;
    case CMT_SET_DISPLAY_POWER:
        *cursor++ = static_cast<char>(m_data.setDisplayPower.on);
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

    if (m_data.type == CMT_INJECT_TEXT) {
        const int length = static_cast<int>(m_data.injectText.length);
        result.resize(5 + length);
        char *cursor = result.data();
        *cursor++ = static_cast<char>(m_data.type);
        write32(cursor, m_data.injectText.length);
        if (length > 0) {
            std::memcpy(cursor, m_data.injectText.text,
                        static_cast<std::size_t>(length));
        }
        return result;
    }

    if (m_data.type == CMT_SET_CLIPBOARD) {
        const int length = static_cast<int>(m_data.setClipboard.length);
        result.resize(14 + length);
        char *cursor = result.data();
        *cursor++ = static_cast<char>(m_data.type);
        write64(cursor, m_data.setClipboard.sequence);
        *cursor++ = static_cast<char>(m_data.setClipboard.paste);
        write32(cursor, m_data.setClipboard.length);
        if (length > 0) {
            std::memcpy(cursor, m_data.setClipboard.text,
                        static_cast<std::size_t>(length));
        }
        return result;
    }

    result.clear();
    return result;
}
