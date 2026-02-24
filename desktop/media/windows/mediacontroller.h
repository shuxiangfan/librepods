#ifndef LIBREPODS_MEDIACONTROLLER_H
#define LIBREPODS_MEDIACONTROLLER_H
#include <QObject>
#include <atomic>
#include "audiocontroller.h"

class QProcess;
class EarDetection;
class PlayerStatusWatcher;
class QDBusInterface;
class QTimer;

class MediaController : public QObject
{
    Q_OBJECT
  public:
    enum MediaState
    {
        Playing,
        Paused,
        Stopped
      };
    Q_ENUM(MediaState)
    enum EarDetectionBehavior
    {
        PauseWhenOneRemoved,
        PauseWhenBothRemoved,
        Disabled
      };
    Q_ENUM(EarDetectionBehavior)

    explicit MediaController(QObject *parent = nullptr);
    ~MediaController();

    void handleEarDetection(EarDetection*);
    void followMediaChanges();
    bool isActiveOutputDeviceAirPods();
    void handleConversationalAwareness(const QByteArray &data);
    void activateA2dpProfile();
    void removeAudioOutputDevice();
    void setConnectedDeviceMacAddress(const QString &macAddress);
    bool isA2dpProfileAvailable();
    QString getPreferredA2dpProfile();
    bool restartWirePlumber();

    void setEarDetectionBehavior(EarDetectionBehavior behavior);
    inline EarDetectionBehavior getEarDetectionBehavior() const { return earDetectionBehavior; }

    void play();
    void pause();
    MediaState getCurrentMediaState() const;

    Q_SIGNALS:
      void mediaStateChanged(MediaState state);

private:
    void pollWindowsPlaybackStateAsync();
    MediaState mediaStateFromPlayerctlOutput(const QString &output) const;
    QString getAudioDeviceName();
    QStringList getPlayingMediaPlayers();

    QStringList pausedByAppServices;
    int initialVolume = -1;
    QString connectedDeviceMacAddress;
    EarDetectionBehavior earDetectionBehavior = PauseWhenOneRemoved;
    QString m_deviceOutputName;
    PlayerStatusWatcher *playerStatusWatcher = nullptr;
    AudioController *m_Audio = nullptr;
    QString m_cachedA2dpProfile;
    MediaState m_currentMediaState = Stopped;
    QTimer *m_windowsPlaybackPollTimer = nullptr;
    std::atomic_bool m_windowsPlaybackQueryInFlight = false;
};
#endif //LIBREPODS_MEDIACONTROLLER_H
