#ifndef LIBREPODS_WINDOWSL2CAPSOCKET_H
#define LIBREPODS_WINDOWSL2CAPSOCKET_H

#include <QObject>
#include <QBluetoothAddress>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QByteArray>

#include <atomic>
#include <mutex>
#include <thread>

class WindowsL2capSocket : public QObject
{
    Q_OBJECT

public:
    explicit WindowsL2capSocket(QObject *parent = nullptr);
    ~WindowsL2capSocket() override;

    void connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid);
    void close();

    bool isOpen() const;
    QBluetoothSocket::SocketState state() const;
    QBluetoothAddress peerAddress() const;
    QString errorString() const;

    QByteArray readAll();
    qint64 write(const QByteArray &data);

signals:
    void connected();
    void readyRead();
    void errorOccurred(QBluetoothSocket::SocketError error);

private:
    using NativeSocket = uintptr_t;

    void setState(QBluetoothSocket::SocketState state);
    void setError(const QString &message, QBluetoothSocket::SocketError error);
    void startReceiveThread();
    void stopReceiveThread();

    static bool ensureWinsockInitialized(QString *errorMessage);
    static bool parseBluetoothAddress(const QBluetoothAddress &address, unsigned long long *outBtAddr);

private:
    mutable std::mutex m_mutex;
    NativeSocket m_socket = static_cast<NativeSocket>(~0ull);
    QBluetoothAddress m_peerAddress;
    QByteArray m_rxBuffer;
    QString m_errorString;
    std::atomic<QBluetoothSocket::SocketState> m_state { QBluetoothSocket::SocketState::UnconnectedState };
    std::atomic_bool m_stopReceiver { false };
    std::thread m_receiveThread;
};

#endif // LIBREPODS_WINDOWSL2CAPSOCKET_H
