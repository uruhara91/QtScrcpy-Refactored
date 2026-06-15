#ifndef FPSCOUNTER_H
#define FPSCOUNTER_H

#include <QObject>
#include <atomic>

class FpsCounter : public QObject
{
    Q_OBJECT
public:
    explicit FpsCounter(QObject *parent = Q_NULLPTR);
    virtual ~FpsCounter();

    void start();
    void stop();
    bool isStarted();
    void addRenderedFrame();
    void addSkippedFrame();

signals:
    void updateFPS(quint32 fps);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    void startCounterTimer();
    void stopCounterTimer();
    void resetCounter();

private:
    qint32 m_counterTimer = 0;
    quint32 m_curRendered = 0;
    quint32 m_curSkipped = 0;

    std::atomic<quint32> m_rendered{0};
    std::atomic<quint32> m_skipped{0};
};

#endif // FPSCOUNTER_H
