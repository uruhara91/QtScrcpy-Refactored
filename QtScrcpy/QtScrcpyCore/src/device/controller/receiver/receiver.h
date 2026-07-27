#ifndef RECEIVER_H
#define RECEIVER_H

#include <QPointer>
#include <QObject>

class DeviceMsg;
class Receiver : public QObject
{
    Q_OBJECT
public:
    explicit Receiver(QObject *parent = nullptr);
    ~Receiver() override;

    void recvDeviceMsg(DeviceMsg *deviceMsg);
};

#endif // RECEIVER_H
