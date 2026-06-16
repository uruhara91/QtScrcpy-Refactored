#ifndef QTSCRCPYTELEMETRY_H
#define QTSCRCPYTELEMETRY_H

#include <QThread>
#include <QtGlobal>

namespace qsc {

[[nodiscard]] inline bool telemetryEnabled() noexcept
{
    static const bool enabled =
        qEnvironmentVariableIntValue("QTSCRCPY_TELEMETRY") > 0;
    return enabled;
}

[[nodiscard]] inline quintptr telemetryThreadId() noexcept
{
    return reinterpret_cast<quintptr>(QThread::currentThreadId());
}

} // namespace qsc

#endif // QTSCRCPYTELEMETRY_H
