#include <QTimerEvent>

#include "fpscounter.h"

FpsCounter::FpsCounter(QObject *parent)
    : QObject(parent)
{
}

void FpsCounter::start()
{
    if (m_counterTimer != 0) return;

    resetCounter();
    m_counterTimer = startTimer(1000, Qt::PreciseTimer);
}

void FpsCounter::stop()
{
    if (m_counterTimer != 0) {
        killTimer(m_counterTimer);
        m_counterTimer = 0;
    }
    resetCounter();
}

bool FpsCounter::isStarted() const noexcept
{
    return m_counterTimer != 0;
}

void FpsCounter::addRenderedFrame() noexcept
{
    m_rendered.fetch_add(1, std::memory_order_relaxed);
}

void FpsCounter::timerEvent(QTimerEvent *event)
{
    if (event && event->timerId() == m_counterTimer) {
        emit updateFPS(m_rendered.exchange(0, std::memory_order_acq_rel));
        return;
    }

    QObject::timerEvent(event);
}

void FpsCounter::resetCounter() noexcept
{
    m_rendered.store(0, std::memory_order_release);
}
