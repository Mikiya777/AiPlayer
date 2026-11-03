#include "ui/PlaybackController.h"
#include "video/VideoItem.h"
#include <QFileInfo>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/error.h>
}

namespace PLAYER {
PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent), m_player(CreateQtAudioOutput())
{
    m_default_audio_device = QMediaDevices::defaultAudioOutput();
    connect(&m_media_devices, &QMediaDevices::audioOutputsChanged,
            this, &PlaybackController::RefreshAudioDevice);
    m_record_timer.setInterval(100);
    connect(&m_record_timer,&QTimer::timeout,this,&PlaybackController::PollRecording);
    m_timer.setInterval(5);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PlaybackController::Advance);
}

PlaybackController::~PlaybackController()
{
    StopRecording();
    m_record_timer.stop();
    m_timer.stop();
    m_player.Close();
}

void PlaybackController::RefreshAudioDevice()
{
    const auto device=QMediaDevices::defaultAudioOutput();
    if(device==m_default_audio_device)return;
    m_default_audio_device=device;
    if(!m_opened || m_at_end)return;
    const bool playing=IsPlaying();
    const double position=m_player.GetPosition();
    // 重新协商 PCM 格式并重置时钟；不能把旧设备格式的数据交给新设备。
    StopRecording();
    m_timer.stop();
    if(m_video_output)m_video_output->ClearFrame();
    if(m_player.Seek(position)<0) {
        const auto error=QString::fromStdString(m_player.GetError());
        m_player.Close();
        m_opened=false;
        ReportError(QStringLiteral("切换音频输出失败：")+error);
        return;
    }
    UpdateVolume();
    if(playing)m_timer.start();
}

void PlaybackController::SetSource(const QUrl& source)
{
    if (m_source == source) return;
    Stop();
    m_source = source;
    m_error.clear();
    if (m_duration != 0) { m_duration = 0; emit DurationChanged(); emit SeekableChanged(); }
    if (m_has_video) { m_has_video = false; emit HasVideoChanged(); }
    emit SourceChanged();
}

bool PlaybackController::EnsureOpen()
{
    if (m_opened) return true;
    if (m_video_output) m_video_output->ClearFrame();
    if (!m_source.isLocalFile() || !QFileInfo::exists(m_source.toLocalFile())) {
        ReportError(QStringLiteral("请选择存在的本地媒体文件"));
        return false;
    }
    if (m_player.Open(m_source.toLocalFile().toStdString()) < 0) {
        ReportError(QString::fromStdString(m_player.GetError()));
        return false;
    }
    m_opened = true;
    if (!m_error.isEmpty()) { m_error.clear(); emit errorOccurred(); }
    m_at_end = false;
    const qint64 duration = qMax<qint64>(0, qRound64(m_player.GetDuration() * 1000.0));
    if (m_duration != duration) {
        m_duration = duration;
        emit DurationChanged();
        emit SeekableChanged();
    }
    const bool hasVideo = m_player.HasVideo();
    if (m_has_video != hasVideo) {
        m_has_video = hasVideo;
        emit HasVideoChanged();
    }
    UpdateVolume();
    return true;
}

void PlaybackController::Play()
{
    if (!EnsureOpen()) return;
    if (m_at_end) {
        SetPosition(0);
        if (!m_opened) return;
    }
    if (m_state == kPausedState && m_player.SetPaused(false) < 0) {
        ReportError(QStringLiteral("恢复播放失败"));
        return;
    }
    SetState(kPlayingState);
    m_timer.start();
}

void PlaybackController::Pause()
{
    if (!m_opened || m_state != kPlayingState) return;
    if (m_player.SetPaused(true) < 0) {
        ReportError(QStringLiteral("暂停播放失败"));
        return;
    }
    m_timer.stop();
    SetState(kPausedState);
}
void PlaybackController::TogglePlayback()
{
    if (IsPlaying()) Pause(); else Play();
}

void PlaybackController::Stop()
{
    StopRecording();
    m_timer.stop();
    m_player.Close();
    m_opened = m_at_end = false;
    SetState(kStoppedState);
    if (m_position != 0) { m_position = 0; emit PositionChanged(); }
    if (m_video_output) m_video_output->ClearFrame();
}

void PlaybackController::SetPosition(qint64 position)
{
    if (m_recording || !EnsureOpen() || !IsSeekable()) return;
    position = std::clamp<qint64>(position, 0, m_duration);
    const bool playing = m_state == kPlayingState;
    m_timer.stop();
    if (m_video_output) m_video_output->ClearFrame();
    if (m_player.Seek(position / 1000.0) < 0) {
        ReportError(QString::fromStdString(m_player.GetError()));
        return;
    }
    m_at_end = false;
    m_position = position;
    emit PositionChanged();
    if (playing) m_timer.start();
}

void PlaybackController::Advance()
{
    TimedFrame video;
    const int ret = m_player.Tick(video);
    if (video.m_frame && m_video_output) {
        video.m_frame->pts=std::llround(video.m_time_seconds*AV_TIME_BASE);
        m_video_output->SetFrame(std::move(video.m_frame));
    }
    const qint64 position = qMax<qint64>(0, qRound64(m_player.GetPosition() * 1000.0));
    if (position != m_position) { m_position = position; emit PositionChanged(); }
    if (ret == AVERROR_EOF) {
        StopRecording();
        m_timer.stop();
        m_at_end = true;
        SetState(kStoppedState);
        emit PlaybackEnded();
        emit playbackFinished();
    } else if (ret < 0) {
        StopRecording();
        ReportError(QString::fromStdString(m_player.GetError()));
    }
}

void PlaybackController::SetVolume(double volume)
{
    if (!std::isfinite(volume)) return;
    volume = std::clamp(volume, 0.0, 1.0);
    if (qFuzzyCompare(m_volume, volume)) return;
    m_volume = volume;
    UpdateVolume();
    emit VolumeChanged();
}

void PlaybackController::SetMuted(bool muted)
{
    if (m_muted == muted) return;
    m_muted = muted;
    UpdateVolume();
    emit MutedChanged();
}

void PlaybackController::UpdateVolume()
{
    const int ret = m_player.SetVolume(m_muted ? 0.0 : m_volume);
    if (ret < 0) ReportError(QStringLiteral("设置音量失败"));
}

void PlaybackController::SetPlaybackRate(double rate)
{
    if (m_recording) return;
    if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0) return;
    if (qFuzzyCompare(rate, GetPlaybackRate())) return;
    const bool playing = IsPlaying();
    m_timer.stop();
    const int ret = m_player.SetPlaybackRate(rate);
    emit PlaybackRateChanged();
    if (ret < 0) {
        m_opened = false;
        ReportError(QString::fromStdString(m_player.GetError()));
        return;
    }
    if (playing) m_timer.start();
}

void PlaybackController::StartRecording(const QUrl& path)
{
    if(m_writer || !m_video_output || !HasVideo())return;
    if(GetPlaybackRate()!=1.0){ReportError(QStringLiteral("请先切换为 1× 倍速再开始录制"));return;}
    if(!path.isLocalFile()){ReportError(QStringLiteral("请选择本地保存路径"));return;}
    const auto output=QFileInfo(path.toLocalFile());
    if(output.absoluteFilePath()==QFileInfo(m_source.toLocalFile()).absoluteFilePath() ||
       (!output.canonicalFilePath().isEmpty() && output.canonicalFilePath()==QFileInfo(m_source.toLocalFile()).canonicalFilePath())) {
        ReportError(QStringLiteral("不能覆盖正在播放的源文件"));return;
    }
    if(output.suffix().compare("mp4",Qt::CaseInsensitive)!=0){ReportError(QStringLiteral("当前请选择 .mp4 文件"));return;}
    const QSize size=m_video_output->GetRecordingSize();
    if(size.isEmpty()){ReportError(QStringLiteral("请等待视频画面出现后再录制"));return;}
    RecordingConfig config;config.m_path=path.toLocalFile().toStdString();config.m_source=m_source.toLocalFile().toStdString();
    config.m_width=size.width();config.m_height=size.height();config.m_start=m_player.GetPosition();
    m_writer=std::make_shared<FileWriter>(std::move(config));
    m_recording=true;m_recording_duration=0;m_video_output->SetRecorder(m_writer);
    m_record_timer.start();emit recordingChanged();
}
void PlaybackController::StopRecording()
{
    if(!m_recording)return;
    m_recording_duration=qMax<qint64>(0,qRound64((m_player.GetPosition()-m_writer->GetConfig().m_start)*1000));
    m_recording=false;
    if(m_video_output)m_video_output->SetRecorder({});
    m_writer->Stop(m_player.GetPosition());emit recordingChanged();
}
void PlaybackController::PollRecording()
{
    if(!m_writer)return;
    if(m_recording){m_recording_duration=qMax<qint64>(0,qRound64((m_player.GetPosition()-m_writer->GetConfig().m_start)*1000));emit recordingChanged();}
    if(!m_writer->IsDone())return;
    const QString error=QString::fromStdString(m_writer->GetError());
    const QString path=QString::fromStdString(m_writer->GetConfig().m_path);
    m_recording=false;if(m_video_output)m_video_output->SetRecorder({});
    m_writer.reset();m_record_timer.stop();emit recordingChanged();
    if(error.isEmpty())emit recordingSaved(path);else ReportError(error);
}

QObject* PlaybackController::GetVideoOutput() const { return m_video_output; }

void PlaybackController::SetVideoOutput(QObject* output)
{
    auto* videoOutput = qobject_cast<VideoItem*>(output);
    if (m_video_output == videoOutput) return;
    QObject::disconnect(m_render_error_connection);
    m_video_output = videoOutput;
    if (videoOutput) {
        m_render_error_connection = connect(videoOutput, &VideoItem::renderFailed, this,
            [this](const QString& error) {
                // 画面无法继续呈现时不让声音独自前进，也不能把失败录制报告为成功。
                if (m_recording && m_writer) m_writer->Fail(error.toStdString());
                StopRecording();
                m_timer.stop();
                m_player.Close();
                m_opened = m_at_end = false;
                ReportError(error);
            });
    }
    emit VideoOutputChanged();
}

void PlaybackController::SetState(PlaybackState state)
{
    if (m_state == state) return;
    m_state = state;
    emit PlaybackStateChanged();
}

void PlaybackController::ReportError(const QString& error)
{
    StopRecording();
    m_timer.stop();
    if (m_opened && m_player.SetPaused(true) < 0) {
        m_player.Close();
        m_opened = m_at_end = false;
    }
    SetState(m_opened ? kPausedState : kStoppedState);
    const auto message = error.isEmpty() ? QStringLiteral("未知播放错误") : error;
    if (m_error != message) {
        m_error = message;
        emit errorOccurred();
    }
}
} // namespace PLAYER
