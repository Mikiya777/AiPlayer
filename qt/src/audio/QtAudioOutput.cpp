#include "audio/IAudioOutput.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <algorithm>

extern "C" {
#include <libavutil/error.h>
}

namespace PLAYER {
namespace {
QAudioFormat::SampleFormat ToQtFormat(AVSampleFormat format) {
  switch (format) {
    case AV_SAMPLE_FMT_U8:
      return QAudioFormat::UInt8;
    case AV_SAMPLE_FMT_S16:
      return QAudioFormat::Int16;
    case AV_SAMPLE_FMT_S32:
      return QAudioFormat::Int32;
    case AV_SAMPLE_FMT_FLT:
      return QAudioFormat::Float;
    default:
      return QAudioFormat::Unknown;
  }
}

class QtAudioOutput final : public IAudioOutput {
public:
  int Open(AudioOutputConfig &config) override {
    Stop();
    const auto device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) return AVERROR(ENODEV);
    QAudioFormat format;
    format.setSampleRate(config.m_sample_rate);
    format.setChannelCount(av_get_channel_layout_nb_channels(config.m_channel_layout));
    format.setSampleFormat(ToQtFormat(config.m_sample_format));
    // 这个教学实现只支持 mono/stereo，避免默默猜测多声道扬声器排列。
    if (!device.isFormatSupported(format)) format = device.preferredFormat();
    if (!format.isValid() || format.channelCount() < 1 || format.channelCount() > 2 ||
        !device.isFormatSupported(format)) return AVERROR(ENOTSUP);
    config.m_sample_rate = format.sampleRate();
    config.m_channel_layout = format.channelCount() == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    switch (format.sampleFormat()) {
      case QAudioFormat::UInt8:
        config.m_sample_format = AV_SAMPLE_FMT_U8;
        break;
      case QAudioFormat::Int16:
        config.m_sample_format = AV_SAMPLE_FMT_S16;
        break;
      case QAudioFormat::Int32:
        config.m_sample_format = AV_SAMPLE_FMT_S32;
        break;
      case QAudioFormat::Float:
        config.m_sample_format = AV_SAMPLE_FMT_FLT;
        break;
      default:
        return AVERROR(ENOTSUP);
    }
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setBufferSize(format.bytesForDuration(100000)); // 100ms，在 start 前设置。
    m_sink->setVolume(m_volume); // 只改变本路声音，不修改系统音量。
    m_device = m_sink->start(); // 借用指针，由 QAudioSink 拥有，不能自己 delete。
    if (!m_device || m_sink->state() == QAudio::StoppedState) {
      Stop();
      return AVERROR(EIO);
    }
    return 0;
  }
  std::int64_t GetFreeBytes() const override {
    return m_sink->bytesFree();
  }
  std::int64_t Write(const uint8_t *data, std::int64_t bytes) override {
    return m_device->write(reinterpret_cast<const char *>(data), bytes);
  }
  double GetProcessedSeconds() const override {
    return m_sink->processedUSecs() / 1000000.0;
  }
  bool IsIdle() const override {
    return m_sink->state() == QAudio::IdleState && m_sink->bytesFree() == m_sink->bufferSize();
  }
  int GetError() const override {
    // 欠载可以通过继续供给 PCM 恢复，不能把它当作致命设备错误。
    return !m_sink || m_sink->state() == QAudio::StoppedState ? AVERROR(EIO) : 0;
  }
  void SetPaused(bool paused) override {
    if (paused)
      m_sink->suspend();
    else
      m_sink->resume();
  }
  void SetVolume(double volume) override {
    m_volume = std::clamp(volume, 0.0, 1.0);
    if (m_sink) m_sink->setVolume(m_volume);
  }
  void Finish() override {
    // 正常结尾使用 stop；Darwin/Linux 后端会排空底层缓冲。
    m_sink->stop();
    m_device = nullptr;
  }
  void Stop() override {
    if (m_sink) m_sink->reset(); // 主动停止/seek 不需要播放旧数据。
    m_device = nullptr;
    m_sink.reset();
  }

private:
  std::unique_ptr<QAudioSink> m_sink;
  QIODevice *m_device = nullptr;
  double m_volume = 0.65;
};
} // namespace
std::unique_ptr<IAudioOutput> CreateQtAudioOutput() {
  return std::make_unique<QtAudioOutput>();
}
} // namespace PLAYER
