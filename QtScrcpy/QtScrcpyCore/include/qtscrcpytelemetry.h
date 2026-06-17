#ifndef QTSCRCPYTELEMETRY_H
#define QTSCRCPYTELEMETRY_H

#include <QThread>
#include <QtGlobal>

namespace qsc::telemetry {

[[nodiscard]] inline bool enabled() noexcept
{
    static const bool value =
        qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;
    return value;
}

[[nodiscard]] inline quintptr threadId() noexcept
{
    return reinterpret_cast<quintptr>(QThread::currentThreadId());
}

[[nodiscard]] inline bool environmentFlag(const char *name,
                                          bool fallback = false) noexcept
{
    if (!name || !qEnvironmentVariableIsSet(name)) return fallback;
    return qEnvironmentVariableIntValue(name) != 0;
}

[[nodiscard]] inline int boundedEnvironmentInt(const char *name,
                                               int fallback,
                                               int minimum,
                                               int maximum) noexcept
{
    if (!name || minimum > maximum) return fallback;

    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? qBound(minimum, value, maximum)
              : qBound(minimum, fallback, maximum);
}

} // namespace qsc::telemetry

namespace qsc {

// Compatibility wrappers for call sites migrated incrementally.
[[nodiscard]] inline bool telemetryEnabled() noexcept
{
    return telemetry::enabled();
}

[[nodiscard]] inline quintptr telemetryThreadId() noexcept
{
    return telemetry::threadId();
}

} // namespace qsc

#endif // QTSCRCPYTELEMETRY_H
