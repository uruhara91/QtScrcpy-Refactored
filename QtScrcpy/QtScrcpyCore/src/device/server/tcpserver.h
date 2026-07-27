#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <QTcpServer>

class TcpServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit TcpServer(QObject *parent = nullptr);
    ~TcpServer() override;

    // Panggil ini tiap kali mulai sequence accept baru (listen() ulang untuk
    // percobaan koneksi baru). m_isVideoSocket cuma di-toggle true->false
    // sekali (video lalu control) dan TIDAK PERNAH direset sendiri walau
    // TcpServer instance ini dipakai lagi untuk percobaan connect
    // berikutnya -- tanpa reset ini, percobaan connect kedua+ akan salah
    // menganggap video socket sebagai control socket (audit §4.5).
    void resetSocketSequence() { m_isVideoSocket = true; }

protected:
    void incomingConnection(qintptr handle) override;

private:
    bool m_isVideoSocket = true;
};

#endif // TCPSERVER_H
