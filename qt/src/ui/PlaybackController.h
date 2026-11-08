#pragma once

#include "core/Player.h"
#include "writer/FileWriter.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>
#include <QMediaDevices>
#include <QAudioDevice>

namespace PLAYER {
class VideoItem;

// QML 与播放器内核之间的适配层。所有控制和 QAudioSink 操作都留在 GUI 线程。
class PlaybackController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool recording READ IsRecording NOTIFY recordingChanged)
  Q_PROPERTY(bool savingRecording READ IsSavingRecording NOTIFY recordingChanged)
  Q_PROPERTY(qint64 recordingDuration READ GetRecordingDuration NOTIFY recordingChanged)
  Q_PROPERTY(QUrl source READ GetSource WRITE SetSource NOTIFY SourceChanged)
  Q_PROPERTY(PlaybackState playbackState READ GetPlaybackState NOTIFY PlaybackStateChanged)
  Q_PROPERTY(bool playing READ IsPlaying NOTIFY PlaybackStateChanged)
  Q_PROPERTY(qint64 position READ GetPosition WRITE SetPosition NOTIFY PositionChanged)
  Q_PROPERTY(qint64 duration READ GetDuration NOTIFY DurationChanged)
  Q_PROPERTY(bool seekable READ IsSeekable NOTIFY SeekableChanged)
  Q_PROPERTY(bool hasVideo READ HasVideo NOTIFY HasVideoChanged)
  Q_PROPERTY(double volume READ GetVolume WRITE SetVolume NOTIFY VolumeChanged)
  Q_PROPERTY(bool muted READ IsMuted WRITE SetMuted NOTIFY MutedChanged)
  Q_PROPERTY(double playbackRate READ GetPlaybackRate WRITE SetPlaybackRate NOTIFY PlaybackRateChanged)
  Q_PROPERTY(QString errorString READ GetErrorString NOTIFY errorOccurred)
  Q_PROPERTY(QObject *videoOutput READ GetVideoOutput WRITE SetVideoOutput NOTIFY VideoOutputChanged)
public:
  enum PlaybackState { kStoppedState,
                       kPlayingState,
                       kPausedState };
  Q_ENUM(PlaybackState)

  explicit PlaybackController(QObject *parent = nullptr);
  ~PlaybackController() override;

  QUrl GetSource() const {
    return m_source;
  }
  void SetSource(const QUrl &source);
  PlaybackState GetPlaybackState() const {
    return m_state;
  }
  bool IsPlaying() const {
    return m_state == kPlayingState;
  }
  qint64 GetPosition() const {
    return m_position;
  }
  void SetPosition(qint64 position);
  qint64 GetDuration() const {
    return m_duration;
  }
  bool IsSeekable() const {
    return m_duration > 0 && m_source.isLocalFile();
  }
  bool HasVideo() const {
    return m_has_video;
  }
  double GetVolume() const {
    return m_volume;
  }
  void SetVolume(double volume);
  bool IsMuted() const {
    return m_muted;
  }
  void SetMuted(bool muted);
  double GetPlaybackRate() const {
    return m_player.GetPlaybackRate();
  }
  void SetPlaybackRate(double rate);
  QString GetErrorString() const {
    return m_error;
  }
  QObject *GetVideoOutput() const;
  void SetVideoOutput(QObject *output);

  bool IsRecording() const {
    return m_recording;
  }
  bool IsSavingRecording() const {
    return m_writer && !m_recording;
  }
  qint64 GetRecordingDuration() const {
    return m_recording_duration;
  }
  Q_INVOKABLE void StartRecording(const QUrl &path);
  Q_INVOKABLE void StopRecording();
  Q_INVOKABLE void Play();
  Q_INVOKABLE void Pause();
  Q_INVOKABLE void Stop();
  Q_INVOKABLE void TogglePlayback();

signals:
  void recordingChanged();
  void recordingSaved(const QString &path);
  void SourceChanged();
  void PlaybackStateChanged();
  void PositionChanged();
  void DurationChanged();
  void SeekableChanged();
  void HasVideoChanged();
  void VolumeChanged();
  void MutedChanged();
  void PlaybackRateChanged();
  void errorOccurred();
  void VideoOutputChanged();
  void PlaybackEnded();
  void playbackFinished(); // QML 信号处理器要求小写起始名称。

private:
  bool EnsureOpen();
  void RefreshAudioDevice();
  void Advance();
  void SetState(PlaybackState state);
  void ReportError(const QString &error);
  void UpdateVolume();

  void PollRecording();
  std::shared_ptr<FileWriter> m_writer;
  QTimer m_record_timer;
  bool m_recording = false;
  qint64 m_recording_duration = 0;
  QMediaDevices m_media_devices;
  QAudioDevice m_default_audio_device;
  Player m_player;
  QTimer m_timer;
  QElapsedTimer m_position_notify_clock;
  int m_position_notify_interval_ms = 50;
  QUrl m_source;
  QPointer<VideoItem> m_video_output;
  QMetaObject::Connection m_render_error_connection;
  PlaybackState m_state = kStoppedState;
  qint64 m_position = 0;
  qint64 m_duration = 0;
  double m_volume = 0.65;
  QString m_error;
  bool m_opened = false;
  bool m_at_end = false;
  bool m_has_video = false;
  bool m_muted = false;
};
} // namespace PLAYER
