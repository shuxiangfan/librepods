#include "windowsl2capsocket.h"
#include "logger.h"

#include <QMetaObject>
#include <QStringList>

#include <winsock2.h>
#include <ws2bth.h>

#include <array>

#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr auto kInvalidSocket = static_cast<SOCKET>(INVALID_SOCKET);
constexpr ULONG kAapPsm = 0x1001;
}

WindowsL2capSocket::WindowsL2capSocket(QObject *parent)
    : QObject(parent)
{
}

WindowsL2capSocket::~WindowsL2capSocket()
{
    close();
}

bool WindowsL2capSocket::ensureWinsockInitialized(QString *errorMessage)
{
    static std::once_flag once;
    static bool ok = false;
    static QString initError;

    std::call_once(once, []() {
        WSADATA wsaData = {};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (rc == 0) {
            ok = true;
            return;
        }
        initError = QStringLiteral("WSAStartup failed: %1").arg(rc);
    });

    if (!ok && errorMessage) {
        *errorMessage = initError;
    }
    return ok;
}

bool WindowsL2capSocket::parseBluetoothAddress(const QBluetoothAddress &address, unsigned long long *outBtAddr)
{
    if (!outBtAddr) {
        return false;
    }

    const QString normalized = address.toString().toUpper();
    const QStringList parts = normalized.split(':');
    if (parts.size() != 6) {
        return false;
    }

    std::array<unsigned int, 6> bytes = {};
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        bytes[static_cast<size_t>(i)] = parts[i].toUInt(&ok, 16);
        if (!ok || bytes[static_cast<size_t>(i)] > 0xFF) {
            return false;
        }
    }

    const unsigned long nap = (bytes[0] << 8) | bytes[1];
    const unsigned long sap = (bytes[2] << 24) | (bytes[3] << 16) | (bytes[4] << 8) | bytes[5];
    *outBtAddr = (static_cast<unsigned long long>(nap) << 32) | sap;
    return true;
}

void WindowsL2capSocket::setState(QBluetoothSocket::SocketState state)
{
    m_state = state;
}

void WindowsL2capSocket::setError(const QString &message, QBluetoothSocket::SocketError error)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_errorString = message;
    }
    QMetaObject::invokeMethod(this, [this, error]() { emit errorOccurred(error); }, Qt::QueuedConnection);
}

void WindowsL2capSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid)
{
    Q_UNUSED(uuid);

    close();

    QString winsockError;
    if (!ensureWinsockInitialized(&winsockError)) {
        setError(winsockError, QBluetoothSocket::SocketError::UnknownSocketError);
        return;
    }

    unsigned long long btAddr = 0;
    if (!parseBluetoothAddress(address, &btAddr)) {
        setError(QStringLiteral("Invalid Bluetooth address: %1").arg(address.toString()),
                 QBluetoothSocket::SocketError::HostNotFoundError);
        return;
    }

    setState(QBluetoothSocket::SocketState::ConnectingState);
    m_peerAddress = address;

    SOCKET s = ::socket(AF_BTH, SOCK_SEQPACKET, BTHPROTO_L2CAP);
    if (s == kInvalidSocket) {
        const int wsaError = WSAGetLastError();
        if (wsaError == WSAESOCKTNOSUPPORT || wsaError == WSAEPROTONOSUPPORT) {
            setState(QBluetoothSocket::SocketState::UnconnectedState);
            setError(QStringLiteral("Windows Winsock Bluetooth L2CAP socket unsupported (AF_BTH/SOCK_SEQPACKET/BTHPROTO_L2CAP), WSA error: %1").arg(wsaError),
                     QBluetoothSocket::SocketError::UnsupportedProtocolError);
            return;
        }

        setState(QBluetoothSocket::SocketState::UnconnectedState);
        setError(QStringLiteral("socket(AF_BTH, L2CAP) failed: %1").arg(wsaError),
                 QBluetoothSocket::SocketError::UnknownSocketError);
        return;
    }

    SOCKADDR_BTH remote = {};
    remote.addressFamily = AF_BTH;
    remote.btAddr = static_cast<BTH_ADDR>(btAddr);
    remote.port = kAapPsm;
    remote.serviceClassId = GUID_NULL;

    if (::connect(s, reinterpret_cast<SOCKADDR *>(&remote), sizeof(remote)) == SOCKET_ERROR) {
        const int wsaError = WSAGetLastError();
        ::closesocket(s);
        setState(QBluetoothSocket::SocketState::UnconnectedState);

        QBluetoothSocket::SocketError qtError = QBluetoothSocket::SocketError::UnknownSocketError;
        if (wsaError == WSAETIMEDOUT) {
            qtError = QBluetoothSocket::SocketError::NetworkError;
        } else if (wsaError == WSAECONNREFUSED) {
            qtError = QBluetoothSocket::SocketError::ServiceNotFoundError;
        } else if (wsaError == WSAEHOSTUNREACH) {
            qtError = QBluetoothSocket::SocketError::HostNotFoundError;
        } else if (wsaError == WSAESOCKTNOSUPPORT || wsaError == WSAEPROTONOSUPPORT) {
            qtError = QBluetoothSocket::SocketError::UnsupportedProtocolError;
        }

        setError(QStringLiteral("Windows L2CAP connect failed (PSM 0x1001), WSA error: %1").arg(wsaError), qtError);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_socket = static_cast<NativeSocket>(s);
        m_errorString.clear();
    }
    setState(QBluetoothSocket::SocketState::ConnectedState);

    startReceiveThread();
    QMetaObject::invokeMethod(this, [this]() { emit connected(); }, Qt::QueuedConnection);
}

void WindowsL2capSocket::startReceiveThread()
{
    stopReceiveThread();

    m_stopReceiver = false;
    m_receiveThread = std::thread([this]() {
        std::array<char, 2048> buffer = {};

        while (!m_stopReceiver) {
            SOCKET s;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                s = static_cast<SOCKET>(m_socket);
            }

            if (s == kInvalidSocket) {
                return;
            }

            const int bytes = ::recv(s, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (bytes > 0) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_rxBuffer.append(buffer.data(), bytes);
                }
                QMetaObject::invokeMethod(this, [this]() { emit readyRead(); }, Qt::QueuedConnection);
                continue;
            }

            if (bytes == 0) {
                setState(QBluetoothSocket::SocketState::UnconnectedState);
                return;
            }

            if (m_stopReceiver) {
                return;
            }

            const int wsaError = WSAGetLastError();
            setState(QBluetoothSocket::SocketState::UnconnectedState);
            setError(QStringLiteral("L2CAP recv failed: %1").arg(wsaError),
                     QBluetoothSocket::SocketError::NetworkError);
            return;
        }
    });
}

void WindowsL2capSocket::stopReceiveThread()
{
    m_stopReceiver = true;
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
}

void WindowsL2capSocket::close()
{
    SOCKET s = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        s = static_cast<SOCKET>(m_socket);
        m_socket = static_cast<NativeSocket>(kInvalidSocket);
        m_rxBuffer.clear();
    }

    if (s != kInvalidSocket) {
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }

    stopReceiveThread();
    setState(QBluetoothSocket::SocketState::UnconnectedState);
}

bool WindowsL2capSocket::isOpen() const
{
    return state() == QBluetoothSocket::SocketState::ConnectedState;
}

QBluetoothSocket::SocketState WindowsL2capSocket::state() const
{
    return m_state.load();
}

QBluetoothAddress WindowsL2capSocket::peerAddress() const
{
    return m_peerAddress;
}

QString WindowsL2capSocket::errorString() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_errorString;
}

QByteArray WindowsL2capSocket::readAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QByteArray out = m_rxBuffer;
    m_rxBuffer.clear();
    return out;
}

qint64 WindowsL2capSocket::write(const QByteArray &data)
{
    SOCKET s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        s = static_cast<SOCKET>(m_socket);
    }

    if (s == kInvalidSocket) {
        return -1;
    }

    const int sent = ::send(s, data.constData(), static_cast<int>(data.size()), 0);
    if (sent == SOCKET_ERROR) {
        const int wsaError = WSAGetLastError();
        setError(QStringLiteral("L2CAP send failed: %1").arg(wsaError),
                 QBluetoothSocket::SocketError::NetworkError);
        return -1;
    }

    return sent;
}
