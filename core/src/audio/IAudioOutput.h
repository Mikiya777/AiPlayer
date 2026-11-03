#pragma once

#include "audio/AudioPipeline.h"
#include <cstdint>
#include <memory>

namespace PLAYER {
// 只隔离设备操作：正式运行使用 QAudioSink，测试时可以换成可控的假设备。
// 所有方法由创建 AudioRenderer 的线程调用，不能从解码线程直接操作设备。
class IAudioOutput {
public:
  virtual ~IAudioOutput() = default;
  // 可修改 config 为设备实际支持的格式，随后 AudioPipeline 必须使用这个配置。
  virtual int Open(AudioOutputConfig &config) = 0;
  virtual std::int64_t GetFreeBytes() const = 0;
  // 返回实际接受的字节数；0=暂时写不进去；负数=错误。
  virtual std::int64_t Write(const uint8_t *data, std::int64_t bytes) = 0;
  virtual double GetProcessedSeconds() const = 0;
  virtual bool IsIdle() const = 0;
  virtual int GetError() const = 0;
  virtual void SetPaused(bool paused) = 0;
  // 0.0 表示静音，1.0 表示原始音量；只控制本播放器，不修改系统音量。
  virtual void SetVolume(double volume) {
    (void)volume;
  }
  virtual void Finish() = 0; // 正常结尾：交由后端完成尾部播放。
  virtual void Stop() = 0; // 用户停止/跳转：立即丢弃缓冲。
};

// 实现在 qt/src/audio/QtAudioOutput.cpp；链接 aiplayer_audio_output 才使用此工厂。
std::unique_ptr<IAudioOutput> CreateQtAudioOutput();
} // namespace PLAYER
