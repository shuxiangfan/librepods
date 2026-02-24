#include "mediacontroller.h"
#include "logger.h"
#include "eardetection.hpp"
#include "audiocontroller.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <optional>
#include <thread>
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

namespace {
using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

bool sendMediaKey(WORD key)
{
  INPUT inputs[2] = {};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = key;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = key;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  return sent == 2;
}

std::optional<MediaController::MediaState> queryWindowsPlaybackState()
{
  try
  {
    using namespace winrt::Windows::Media::Control;

    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    auto session = manager.GetCurrentSession();
    if (!session)
    {
      return MediaController::Stopped;
    }

    auto playbackInfo = session.GetPlaybackInfo();
    PlaybackStatus status = playbackInfo.PlaybackStatus();

    switch (status)
    {
    case PlaybackStatus::Playing:
      return MediaController::Playing;
    case PlaybackStatus::Paused:
      return MediaController::Paused;
    case PlaybackStatus::Stopped:
    case PlaybackStatus::Closed:
      return MediaController::Stopped;
    default:
      return MediaController::Stopped;
    }
  }
  catch (const winrt::hresult_error &e)
  {
    qWarning() << "GSMTC query failed:" << QString::fromWCharArray(e.message().c_str());
  }
  catch (...)
  {
    qWarning() << "GSMTC query failed with unknown error";
  }

  return std::nullopt;
}
}

MediaController::MediaController(QObject *parent) : QObject(parent) {
  try
  {
    winrt::init_apartment();
  }
  catch (const winrt::hresult_error &e)
  {
    if (e.code() == RPC_E_CHANGED_MODE)
    {
      LOG_DEBUG("WinRT apartment already initialized with a different mode");
    }
    else
    {
      LOG_WARN("WinRT apartment initialization failed: " << QString::fromWCharArray(e.message().c_str()));
    }
  }

  m_Audio = new AudioController(this);
  if (!m_Audio->initialize())
  {
    LOG_ERROR("Failed to initialize Audio controller");
  }
}

void MediaController::handleEarDetection(EarDetection *earDetection)
{
  if (earDetectionBehavior == Disabled)
  {
    LOG_DEBUG("Ear detection is disabled, ignoring status");
    return;
  }

  bool primaryInEar = earDetection->isPrimaryInEar();
  bool secondaryInEar = earDetection->isSecondaryInEar();

  LOG_DEBUG("Ear detection status: primaryInEar="
            << primaryInEar << ", secondaryInEar=" << secondaryInEar
            << ", isAirPodsActive=" << isActiveOutputDeviceAirPods());

  // First handle playback pausing based on selected behavior
  bool shouldPause = false;
  bool shouldResume = false;

  if (earDetectionBehavior == PauseWhenOneRemoved)
  {
    shouldPause = !primaryInEar || !secondaryInEar;
    shouldResume = primaryInEar && secondaryInEar;
  }
  else if (earDetectionBehavior == PauseWhenBothRemoved)
  {
    shouldPause = !primaryInEar && !secondaryInEar;
    shouldResume = primaryInEar || secondaryInEar;
  }

  if (shouldPause && isActiveOutputDeviceAirPods())
  {
    if (getCurrentMediaState() == Playing)
    {
      LOG_DEBUG("Pausing playback for ear detection");
      pause();
    }
  }

  // Then handle device profile switching
  if (primaryInEar || secondaryInEar)
  {
    LOG_INFO("At least one AirPod is in ear");
    activateA2dpProfile();

    // Resume if conditions are met and we previously paused
    if (shouldResume && !pausedByAppServices.isEmpty() && isActiveOutputDeviceAirPods())
    {
      play();
    }
  }
  else
  {
    LOG_INFO("Both AirPods are out of ear");
    removeAudioOutputDevice();
  }
}

void MediaController::setEarDetectionBehavior(EarDetectionBehavior behavior)
{
  earDetectionBehavior = behavior;
  LOG_INFO("Set ear detection behavior to: " << behavior);
}

void MediaController::followMediaChanges() {
  if (m_windowsPlaybackPollTimer) {
    return;
  }

  m_windowsPlaybackPollTimer = new QTimer(this);
  m_windowsPlaybackPollTimer->setInterval(750);
  connect(m_windowsPlaybackPollTimer, &QTimer::timeout, this, [this]() { pollWindowsPlaybackStateAsync(); });
  m_windowsPlaybackPollTimer->start();

  pollWindowsPlaybackStateAsync();
  LOG_INFO("Windows GSMTC playback watcher started");
  emit mediaStateChanged(m_currentMediaState);
}

void MediaController::pollWindowsPlaybackStateAsync()
{
  if (m_windowsPlaybackQueryInFlight.exchange(true))
  {
    return;
  }

  QPointer<MediaController> self(this);
  std::thread([self]() {
    std::optional<MediaController::MediaState> state;
    try
    {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...) { }

    state = queryWindowsPlaybackState();

    if (self)
    {
      QMetaObject::invokeMethod(self, [self, state]() {
        if (!self)
        {
          return;
        }

        self->m_windowsPlaybackQueryInFlight = false;

        if (!state)
        {
          return;
        }

        if (*state != self->m_currentMediaState)
        {
          self->m_currentMediaState = *state;
          LOG_DEBUG("Windows playback state changed to " << self->m_currentMediaState);
          emit self->mediaStateChanged(self->m_currentMediaState);
        }
      }, Qt::QueuedConnection);
    }
  }).detach();
}

bool MediaController::isActiveOutputDeviceAirPods() {
  QString defaultSink = m_Audio->getDefaultSink();
  LOG_DEBUG("Default sink: " << defaultSink);

  if (defaultSink.isEmpty()) {
    return false;
  }

  if (!connectedDeviceMacAddress.isEmpty() && defaultSink.contains(connectedDeviceMacAddress, Qt::CaseInsensitive)) {
    return true;
  }

  // Windows endpoint identifiers typically do not include the Bluetooth MAC.
  return !m_deviceOutputName.isEmpty() && defaultSink == m_deviceOutputName;
}

void MediaController::handleConversationalAwareness(const QByteArray &data) {
    if (data.size() < 10) {
        LOG_ERROR("Invalid conversational awareness packet");
        return;
    }

    uint8_t flag = (uint8_t)data[9];

    switch (flag) {
    case 0x01:
        LOG_INFO("Conversational awareness event: voice detected");

        if (initialVolume == -1 && isActiveOutputDeviceAirPods()) {
            QString sink = m_Audio->getDefaultSink();
            initialVolume = m_Audio->getSinkVolume(sink);
            LOG_DEBUG("Initial volume saved: " << initialVolume << "%");
        }

        if (initialVolume != -1) {
            QString sink = m_Audio->getDefaultSink();
            int target = initialVolume * 0.20;
            m_Audio->setSinkVolume(sink, target);
            LOG_INFO("Volume lowered to " << target << "%");
        }
        break;

    case 0x08:
        LOG_INFO("Conversational awareness disabled");
        initialVolume = -1;
        break;

    case 0x09:
        LOG_INFO("Conversational awareness enabled");
        break;

    default:
        LOG_INFO("Conversational awareness event: voice ended");

        if (initialVolume != -1 && isActiveOutputDeviceAirPods()) {
            QString sink = m_Audio->getDefaultSink();
            m_Audio->setSinkVolume(sink, initialVolume);
            LOG_INFO("Volume restored to " << initialVolume << "%");
            initialVolume = -1;
        }
        break;
    }
}


bool MediaController::isA2dpProfileAvailable() {
  if (m_deviceOutputName.isEmpty()) {
    return false;
  }

  return m_Audio->isProfileAvailable(m_deviceOutputName, "a2dp-sink-sbc_xq") ||
         m_Audio->isProfileAvailable(m_deviceOutputName, "a2dp-sink-sbc") ||
         m_Audio->isProfileAvailable(m_deviceOutputName, "a2dp-sink");
}

QString MediaController::getPreferredA2dpProfile() {
  if (m_deviceOutputName.isEmpty()) {
    return QString();
  }

  if (!m_cachedA2dpProfile.isEmpty() &&
      m_Audio->isProfileAvailable(m_deviceOutputName, m_cachedA2dpProfile)) {
    return m_cachedA2dpProfile;
  }

  QStringList profiles = {"a2dp-sink-sbc_xq", "a2dp-sink-sbc", "a2dp-sink"};

  for (const QString &profile : profiles) {
    if (m_Audio->isProfileAvailable(m_deviceOutputName, profile)) {
      LOG_INFO("Selected best available A2DP profile: " << profile);
      m_cachedA2dpProfile = profile;
      return profile;
    }
  }

  m_cachedA2dpProfile.clear();
  return QString();
}

bool MediaController::restartWirePlumber() {
  LOG_INFO("restartWirePlumber() is not applicable on Windows");
  return false;
}

void MediaController::activateA2dpProfile() {
  if (connectedDeviceMacAddress.isEmpty() || m_deviceOutputName.isEmpty()) {
    LOG_WARN("Connected device MAC address or output name is empty, cannot activate A2DP profile");
    return;
  }

  if (!isA2dpProfileAvailable()) {
    LOG_WARN("A2DP profile abstraction unavailable on Windows backend");
    return;
  }

  QString preferredProfile = getPreferredA2dpProfile();
  if (preferredProfile.isEmpty()) {
    LOG_ERROR("No suitable A2DP profile found");
    return;
  }

  LOG_INFO("Activating A2DP profile for AirPods: " << preferredProfile);
  if (!m_Audio->setCardProfile(m_deviceOutputName, preferredProfile)) {
    LOG_WARN("Windows backend does not expose PulseAudio-style profile switching; continuing");
    return;
  }
  LOG_INFO("A2DP profile activated successfully");
}

void MediaController::removeAudioOutputDevice() {
  if (connectedDeviceMacAddress.isEmpty() || m_deviceOutputName.isEmpty()) {
    LOG_WARN("Connected device MAC address or output name is empty, cannot remove audio output device");
    return;
  }

  LOG_INFO("Removing AirPods as audio output device");
  if (!m_Audio->setCardProfile(m_deviceOutputName, "off")) {
    LOG_WARN("Windows backend does not support disabling endpoint via PulseAudio profile API");
  }
}

void MediaController::setConnectedDeviceMacAddress(const QString &macAddress) {
  connectedDeviceMacAddress = macAddress;
  m_deviceOutputName = getAudioDeviceName();
  m_cachedA2dpProfile.clear();
  LOG_INFO("Device output name set to: " << m_deviceOutputName);
}

MediaController::MediaState MediaController::mediaStateFromPlayerctlOutput(
    const QString &output) const {
  if (output == "Playing") {
    return MediaState::Playing;
  } else if (output == "Paused") {
    return MediaState::Paused;
  } else {
    return MediaState::Stopped;
  }
}

MediaController::MediaState MediaController::getCurrentMediaState() const
{
  return m_currentMediaState;
}

QStringList MediaController::getPlayingMediaPlayers()
{
  LOG_DEBUG("getPlayingMediaPlayers() is not implemented on Windows backend");
  return {};
}

void MediaController::play()
{
  if (pausedByAppServices.isEmpty())
  {
    LOG_INFO("No services to resume");
    return;
  }

  if (!sendMediaKey(VK_MEDIA_PLAY_PAUSE))
  {
    LOG_ERROR("Failed to send media play/pause key");
    return;
  }

  m_currentMediaState = Playing;
  pausedByAppServices.clear();
  LOG_INFO("Resumed media playback via Windows media key");
  emit mediaStateChanged(m_currentMediaState);
}

void MediaController::pause()
{
  pausedByAppServices.clear();

  if (!sendMediaKey(VK_MEDIA_PLAY_PAUSE))
  {
    LOG_ERROR("Failed to send media play/pause key");
    return;
  }

  // Keep Linux-compatible resume flow: a non-empty list means "we paused something".
  pausedByAppServices << "windows-media-key";
  m_currentMediaState = Paused;
  LOG_INFO("Paused media playback via Windows media key");
  emit mediaStateChanged(m_currentMediaState);
}

MediaController::~MediaController() {
}

QString MediaController::getAudioDeviceName()
{
  if (connectedDeviceMacAddress.isEmpty()) { return QString(); }

  QString cardName = m_Audio->getCardNameForDevice(connectedDeviceMacAddress);
  if (cardName.isEmpty()) {
    LOG_WARN("No matching Windows audio endpoint found for device MAC: " << connectedDeviceMacAddress);
  }
  return cardName;
}
