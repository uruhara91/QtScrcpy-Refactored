#include <QDebug>
#include <QTimerEvent>

#include "fpscounter.h"

FpsCounter::FpsCounter(QObject *parent) : QObject(parent) {}

FpsCounter::~FpsCounter() {}

void FpsCounter::start()
{
    resetCounter();
    startCounterTimer();
}

void FpsCounter::stop()
{
    stopCounterTimer();
    resetCounter();
}

bool FpsCounter::isStarted()
{
    return m_counterTimer;
}

void FpsCounter::addRenderedFrame()
{
    m_rendered.fetch_add(1, std::memory_order_relaxed);
}

void FpsCounter::addSkippedFrame()
{
    m_skipped.fetch_add(1, std::memory_order_relaxed);
}

void FpsCounter::timerEvent(QTimerEvent *event)
{
    if (event && m_counterTimer == event->timerId()) {
        m_curRendered = m_rendered.exchange(0, std::memory_order_acq_rel);
        m_curSkipped = m_skipped.exchange(0, std::memory_order_acq_rel);
        emit updateFPS(m_curRendered);
        //qInfo("FPS:%d Discard:%d", m_curRendered, m_curSkipped);
    }
}

void FpsCounter::startCounterTimer()
{
    stopCounterTimer();
    m_counterTimer = startTimer(1000);
}

void FpsCounter::stopCounterTimer()
{
    if (m_counterTimer) {
        killTimer(m_counterTimer);
        m_counterTimer = 0;
    }
}

void FpsCounter::resetCounter()
{
    m_rendered.store(0, std::memory_order_release);
    m_skipped.store(0, std::memory_order_release);
}
