#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <memory>

class ScrcpyAudioWorker : public QObject
{
    Q_OBJECT
public:
    explicit ScrcpyAudioWorker(QObject *parent = nullptr);
    ~ScrcpyAudioWorker() override;

public slots:
    void startSession(const QString &serial,
                      quint16 localPort,
                      const QString &serverPath,
                      const QString &serverVersion);
    void stopSession();

signals:
    void started();
    void stopped();
    void errorOccurred(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput() override;

    // Starts a dedicated scrcpy-server audio-only session. The request is
    // asynchronous; completion is reported through started()/errorOccurred().
    bool start(const QString &serial, int port = 28200);
    void stop();

signals:
    void started();
    void stopped();
    void errorOccurred(const QString &message);

private:
    [[nodiscard]] QString resolveServerPath() const;
    [[nodiscard]] QString resolveServerVersion() const;

private:
    QThread m_workerThread;
    QPointer<ScrcpyAudioWorker> m_worker;
};

#endif // AUDIOOUTPUT_H
