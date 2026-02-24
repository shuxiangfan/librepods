#ifndef LIBREPODS_AUDIOCONTROLLER_H
#define LIBREPODS_AUDIOCONTROLLER_H

#include <QString>
#include <QObject>

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioEndpointVolume;

class AudioController : public QObject
{
    Q_OBJECT

public:
    explicit AudioController(QObject *parent = nullptr);
    ~AudioController();

    bool initialize();
    QString getDefaultSink();
    int getSinkVolume(const QString &sinkName);
    bool setSinkVolume(const QString &sinkName, int volumePercent);
    bool setCardProfile(const QString &cardName, const QString &profileName);
    QString getCardNameForDevice(const QString &macAddress);
    bool isProfileAvailable(const QString &cardName, const QString &profileName);

private:
    bool refreshDefaultEndpoint();
    void releaseAudioObjects();
    QString currentDefaultEndpointId() const;

private:
    IMMDeviceEnumerator *m_enumerator = nullptr;
    IMMDevice *m_defaultDevice = nullptr;
    IAudioEndpointVolume *m_endpointVolume = nullptr;
    bool m_initialized = false;
    QString m_defaultEndpointId;
};

#endif //LIBREPODS_AUDIOCONTROLLER_H