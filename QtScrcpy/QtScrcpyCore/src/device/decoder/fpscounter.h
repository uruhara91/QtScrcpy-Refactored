#ifndef FPSCOUNTER_H
#define FPSCOUNTER_H

#include <QObject>
#include <atomic>

class FpsCounter : public QObject
{
    Q_OBJECT
public:
    explicit FpsCounter(QObject *parent = nullptr);
    ~FpsCounter() override = default;

    void start();
    void stop();
    [[nodiscard]] bool isStarted() const noexcept;
    void addRenderedFrame() noexcept;

signals:
    void updateFPS(quint32 fps);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    void resetCounter() noexcept;

private:
    qint32 m_counterTimer = 0;
    std::atomic<quint32> m_rendered{0};
};

#endif // FPSCOUNTER_H
