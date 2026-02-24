#ifndef LIBREPODS_BUMBLEAACPSOCKET_H
#define LIBREPODS_BUMBLEAACPSOCKET_H

#include <QObject>
#include <QBluetoothAddress>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QByteArray>
#include <QProcess>
#include <QVariantMap>

class BumbleAacpSocket : public QObject
{
    Q_OBJECT

public:
    explicit BumbleAacpSocket(QObject *parent = nullptr);
    ~BumbleAacpSocket() override;

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

private slots:
    void onProcessReadyRead();
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void setState(QBluetoothSocket::SocketState state);
    void setError(const QString &message, QBluetoothSocket::SocketError error);
    void sendJsonCommand(const QString &cmd, const QVariantMap &payload = {});
    void handleJsonLine(const QByteArray &line);
    QString resolveHelperScriptPath() const;
    QString resolvePythonExecutable() const;

private:
    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QByteArray m_rxBuffer;
    QBluetoothAddress m_peerAddress;
    QString m_errorString;
    QBluetoothSocket::SocketState m_state = QBluetoothSocket::SocketState::UnconnectedState;
};

#endif // LIBREPODS_BUMBLEAACPSOCKET_H
