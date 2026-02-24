#include "bumbleaacpsocket.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcessEnvironment>

namespace {
QBluetoothSocket::SocketError mapProcessError(QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        return QBluetoothSocket::SocketError::UnsupportedProtocolError;
    default:
        return QBluetoothSocket::SocketError::UnknownSocketError;
    }
}
}

BumbleAacpSocket::BumbleAacpSocket(QObject *parent)
    : QObject(parent)
{
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &BumbleAacpSocket::onProcessReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray err = m_process->readAllStandardError();
        m_stderrBuffer.append(err);
        if (!err.trimmed().isEmpty()) {
            qWarning().noquote() << "[BumbleHelper]" << QString::fromUtf8(err).trimmed();
        }
    });
    connect(m_process, &QProcess::errorOccurred, this, &BumbleAacpSocket::onProcessError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BumbleAacpSocket::onProcessFinished);
}

BumbleAacpSocket::~BumbleAacpSocket()
{
    close();
}

void BumbleAacpSocket::setState(QBluetoothSocket::SocketState state)
{
    m_state = state;
}

void BumbleAacpSocket::setError(const QString &message, QBluetoothSocket::SocketError error)
{
    m_errorString = message;
    emit errorOccurred(error);
}

QString BumbleAacpSocket::resolvePythonExecutable() const
{
    const QString envPython = qEnvironmentVariable("LIBREPODS_BUMBLE_HELPER_PYTHON");
    if (!envPython.isEmpty()) {
        return envPython;
    }
    return QStringLiteral("python");
}

QString BumbleAacpSocket::resolveHelperScriptPath() const
{
    const QString envPath = qEnvironmentVariable("LIBREPODS_BUMBLE_HELPER_SCRIPT");
    if (!envPath.isEmpty()) {
        return envPath;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString localCopy = QDir(appDir).filePath("bumble_aacp_helper.py");
    if (QFileInfo::exists(localCopy)) {
        return localCopy;
    }

    // Fallback for running from source tree without post-build copy
    const QString sourceRelative = QDir(appDir).filePath("../../media/windows/bumble_aacp_helper.py");
    return QDir::cleanPath(sourceRelative);
}

void BumbleAacpSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid)
{
    Q_UNUSED(uuid);
    close();

    const QString pythonExe = resolvePythonExecutable();
    const QString helperScript = resolveHelperScriptPath();

    if (!QFileInfo::exists(helperScript)) {
        setError(QStringLiteral("Bumble helper script not found: %1").arg(helperScript),
                 QBluetoothSocket::SocketError::UnknownSocketError);
        return;
    }

    m_peerAddress = address;
    m_errorString.clear();
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_rxBuffer.clear();
    setState(QBluetoothSocket::SocketState::ConnectingState);

    QStringList args;
    args << helperScript;

    m_process->start(pythonExe, args);
    if (!m_process->waitForStarted(5000)) {
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        setError(QStringLiteral("Failed to start Bumble helper process"),
                 QBluetoothSocket::SocketError::UnsupportedProtocolError);
        return;
    }

    sendJsonCommand(QStringLiteral("connect"), {{"bdaddr", address.toString()}});
}

void BumbleAacpSocket::sendJsonCommand(const QString &cmd, const QVariantMap &payload)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    QJsonObject obj = QJsonObject::fromVariantMap(payload);
    obj.insert(QStringLiteral("cmd"), cmd);
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    m_process->write(line);
}

void BumbleAacpSocket::onProcessReadyRead()
{
    m_stdoutBuffer.append(m_process->readAllStandardOutput());

    int newlineIndex = -1;
    while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_stdoutBuffer.left(newlineIndex).trimmed();
        m_stdoutBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            handleJsonLine(line);
        }
    }
}

void BumbleAacpSocket::handleJsonLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning().noquote() << "Invalid Bumble helper JSON:" << QString::fromUtf8(line);
        return;
    }

    const QJsonObject obj = doc.object();
    const QString event = obj.value(QStringLiteral("event")).toString();

    if (event == QStringLiteral("connected")) {
        setState(QBluetoothSocket::SocketState::ConnectedState);
        emit connected();
        return;
    }

    if (event == QStringLiteral("aacp_pdu")) {
        const QByteArray data = QByteArray::fromHex(obj.value(QStringLiteral("hex")).toString().toUtf8());
        if (!data.isEmpty()) {
            m_rxBuffer.append(data);
            emit readyRead();
        }
        return;
    }

    if (event == QStringLiteral("disconnected")) {
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        return;
    }

    if (event == QStringLiteral("error")) {
        const QString message = obj.value(QStringLiteral("message")).toString(QStringLiteral("Bumble helper error"));
        const QString code = obj.value(QStringLiteral("code")).toString();
        QBluetoothSocket::SocketError err = QBluetoothSocket::SocketError::UnknownSocketError;
        if (code == QStringLiteral("missing_bumble") || code == QStringLiteral("failed_to_start")) {
            err = QBluetoothSocket::SocketError::UnsupportedProtocolError;
        } else if (code == QStringLiteral("connect_failed")) {
            err = QBluetoothSocket::SocketError::NetworkError;
        }
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        setError(message, err);
        return;
    }
}

void BumbleAacpSocket::onProcessError(QProcess::ProcessError error)
{
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    setError(QStringLiteral("Bumble helper process error: %1").arg(static_cast<int>(error)),
             mapProcessError(error));
}

void BumbleAacpSocket::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    if (m_state != QBluetoothSocket::SocketState::UnconnectedState) {
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        if (exitCode != 0) {
            const QString stderrText = QString::fromUtf8(m_stderrBuffer);
            if (exitCode == 9009 || stderrText.contains(QStringLiteral("Python was not found"), Qt::CaseInsensitive)) {
                setError(QStringLiteral("Python runtime not found for Bumble helper. Install Python and Bumble, or set LIBREPODS_BUMBLE_HELPER_PYTHON."),
                         QBluetoothSocket::SocketError::UnsupportedProtocolError);
            } else {
                setError(QStringLiteral("Bumble helper exited with code %1").arg(exitCode),
                         QBluetoothSocket::SocketError::UnknownSocketError);
            }
        }
    }
}

void BumbleAacpSocket::close()
{
    if (m_process && m_process->state() == QProcess::Running) {
        sendJsonCommand(QStringLiteral("disconnect"));
        m_process->waitForBytesWritten(1000);
        m_process->terminate();
        if (!m_process->waitForFinished(3000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    m_rxBuffer.clear();
}

bool BumbleAacpSocket::isOpen() const
{
    return m_state == QBluetoothSocket::SocketState::ConnectedState;
}

QBluetoothSocket::SocketState BumbleAacpSocket::state() const
{
    return m_state;
}

QBluetoothAddress BumbleAacpSocket::peerAddress() const
{
    return m_peerAddress;
}

QString BumbleAacpSocket::errorString() const
{
    return m_errorString;
}

QByteArray BumbleAacpSocket::readAll()
{
    const QByteArray out = m_rxBuffer;
    m_rxBuffer.clear();
    return out;
}

qint64 BumbleAacpSocket::write(const QByteArray &data)
{
    if (!isOpen()) {
        return -1;
    }
    sendJsonCommand(QStringLiteral("aacp_send"), {{"hex", QString::fromUtf8(data.toHex())}});
    return data.size();
}
