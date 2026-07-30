#ifndef BUFFERUTIL_H
#define BUFFERUTIL_H

#include <QtGlobal>
#include <type_traits>

// Big-endian read/write helpers for scrcpy's binary wire protocol (control
// messages sent to the device, device messages -- clipboard sync etc. --
// received back, and the initial connection handshake).
//
// This consolidates what used to be four independent, near-identical
// implementations of the exact same big-endian packing logic scattered
// across controlmsg.cpp (write side), devicemsg.cpp and server.cpp (read
// side), plus a fifth, unused QBuffer-based version that used to live here.
// That QBuffer-based version was dead code (nothing called it) and not a
// good fit to revive as-is: QBuffer::putChar()/getChar() go through
// QIODevice's virtual dispatch per byte, which is real overhead for
// write16()/write32() specifically, since those run on every single mouse
// move and touch event sent to the device. The functions below match the
// faster, allocation-free cursor/pointer style controlmsg.cpp had already
// converged on, so nothing on that hot path gets slower by reusing them.
//
// read16/read32/read64 are templated on the byte pointer type (`const
// char*` from QByteArray::constData(), `const quint8*` elsewhere, etc.)
// so callers never need a cast just to match this API.
namespace BufferUtil {

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

template <typename Byte>
    requires (sizeof(Byte) == 1)
[[nodiscard]] constexpr quint16 read16(const Byte *data) noexcept
{
    return (static_cast<quint16>(static_cast<quint8>(data[0])) << 8) |
            static_cast<quint16>(static_cast<quint8>(data[1]));
}

template <typename Byte>
    requires (sizeof(Byte) == 1)
[[nodiscard]] constexpr quint32 read32(const Byte *data) noexcept
{
    return (static_cast<quint32>(static_cast<quint8>(data[0])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(data[1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(data[2])) << 8) |
            static_cast<quint32>(static_cast<quint8>(data[3]));
}

template <typename Byte>
    requires (sizeof(Byte) == 1)
[[nodiscard]] constexpr quint64 read64(const Byte *data) noexcept
{
    return (static_cast<quint64>(read32(data)) << 32) |
            static_cast<quint64>(read32(data + 4));
}

} // namespace BufferUtil

#endif // BUFFERUTIL_H
