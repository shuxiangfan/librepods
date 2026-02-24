#include "audiocontroller.h"
#include "logger.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <combaseapi.h>

#pragma comment(lib, "Ole32.lib")

AudioController::AudioController(QObject *parent)
    : QObject(parent)
{
}

AudioController::~AudioController()
{
    releaseAudioObjects();

    CoUninitialize();
}

void AudioController::releaseAudioObjects()
{
    if (m_endpointVolume)
    {
        m_endpointVolume->Release();
        m_endpointVolume = nullptr;
    }
    if (m_defaultDevice)
    {
        m_defaultDevice->Release();
        m_defaultDevice = nullptr;
    }
    if (m_enumerator)
    {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
    m_defaultEndpointId.clear();
    m_initialized = false;
}

bool AudioController::initialize()
{
    releaseAudioObjects();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_ERROR("CoInitializeEx failed");
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void **>(&m_enumerator));
    if (FAILED(hr) || !m_enumerator)
    {
        LOG_ERROR("Failed to create MMDeviceEnumerator");
        return false;
    }

    if (!refreshDefaultEndpoint())
    {
        LOG_ERROR("Failed to get default audio endpoint");
        return false;
    }

    m_initialized = true;
    LOG_INFO("Windows AudioController initialized");
    return true;
}

bool AudioController::refreshDefaultEndpoint()
{
    if (!m_enumerator)
        return false;

    if (m_endpointVolume)
    {
        m_endpointVolume->Release();
        m_endpointVolume = nullptr;
    }
    if (m_defaultDevice)
    {
        m_defaultDevice->Release();
        m_defaultDevice = nullptr;
    }
    m_defaultEndpointId.clear();

    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_defaultDevice);
    if (FAILED(hr) || !m_defaultDevice)
    {
        LOG_ERROR("GetDefaultAudioEndpoint failed");
        return false;
    }

    LPWSTR id = nullptr;
    hr = m_defaultDevice->GetId(&id);
    if (SUCCEEDED(hr) && id)
    {
        m_defaultEndpointId = QString::fromWCharArray(id);
        CoTaskMemFree(id);
    }

    hr = m_defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                   nullptr, reinterpret_cast<void **>(&m_endpointVolume));
    if (FAILED(hr) || !m_endpointVolume)
    {
        LOG_ERROR("Activate IAudioEndpointVolume failed");
        return false;
    }

    return true;
}

QString AudioController::currentDefaultEndpointId() const
{
    return m_defaultEndpointId;
}

QString AudioController::getDefaultSink()
{
    if (!m_initialized && !initialize())
        return QString();

    return currentDefaultEndpointId();
}

int AudioController::getSinkVolume(const QString &sinkName)
{
    if (!m_initialized && !initialize())
        return -1;

    if (!sinkName.isEmpty() && sinkName != m_defaultEndpointId)
    {
        refreshDefaultEndpoint();
    }
    else if (!m_endpointVolume)
    {
        if (!refreshDefaultEndpoint())
            return -1;
    }

    float scalar = 0.0f;
    HRESULT hr = m_endpointVolume->GetMasterVolumeLevelScalar(&scalar);
    if (FAILED(hr))
    {
        LOG_ERROR("GetMasterVolumeLevelScalar failed");
        return -1;
    }

    int vol = static_cast<int>(scalar * 100.0f + 0.5f);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return vol;
}

bool AudioController::setSinkVolume(const QString &sinkName, int volumePercent)
{
    if (!m_initialized && !initialize())
        return false;

    if (volumePercent < 0) volumePercent = 0;
    if (volumePercent > 100) volumePercent = 100;

    // TODO:初期策略：仅支持默认输出设备
    if (!sinkName.isEmpty() && sinkName != m_defaultEndpointId)
    {
        refreshDefaultEndpoint();
    }
    else if (!m_endpointVolume)
    {
        if (!refreshDefaultEndpoint())
            return false;
    }

    const float scalar = static_cast<float>(volumePercent) / 100.0f;
    HRESULT hr = m_endpointVolume->SetMasterVolumeLevelScalar(scalar, nullptr);
    if (FAILED(hr))
    {
        LOG_ERROR("SetMasterVolumeLevelScalar failed");
        return false;
    }

    return true;
}

bool AudioController::setCardProfile(const QString &cardName, const QString &profileName)
{
    Q_UNUSED(cardName);

    // Windows does not expose PulseAudio card profiles. Treat A2DP profile
    // selections as successful no-ops so the shared MediaController flow can
    // continue without Linux-specific failure handling.
    if (profileName.startsWith("a2dp-sink"))
    {
        LOG_INFO("Ignoring PulseAudio profile switch on Windows backend: " << profileName);
        return true;
    }

    if (profileName == "off")
    {
        LOG_WARN("Endpoint disable via PulseAudio profile is not supported on Windows backend");
        return false;
    }

    LOG_WARN("Unknown profile request on Windows backend: " << profileName);
    return false;
}

QString AudioController::getCardNameForDevice(const QString &macAddress)
{
    Q_UNUSED(macAddress);
    // Best-effort compatibility: use the current default endpoint ID as the
    // "card name" placeholder expected by the shared media controller.
    QString endpointId = getDefaultSink();
    if (endpointId.isEmpty())
    {
        LOG_WARN("No default Windows audio endpoint available");
        return QString();
    }
    return endpointId;
}

bool AudioController::isProfileAvailable(const QString &cardName, const QString &profileName)
{
    if (cardName.isEmpty())
        return false;

    // Windows does not expose Linux profile names. Report common A2DP profile
    // probes as available so shared logic can proceed.
    if (profileName.startsWith("a2dp-sink"))
        return true;

    return false;
}
