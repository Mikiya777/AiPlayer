#pragma once

#include "core/MediaTypes.h"
#include <atomic>
#include "decoder/AudioDecoder.h"
#include "decoder/VideoDecoder.h"
#include "reader/Demuxer.h"

namespace PLAYER {
// 将解封装和解码串起来的同步读取器：一个输入，音视频各一个解码器。
class FileReader {
public:
  FileReader() = default;
  ~FileReader();
  FileReader(const FileReader &) = delete;
  FileReader &operator=(const FileReader &) = delete;

  // 允许纯音频或纯视频输入；每种类型选择一条流，其他流跳过。
  // 可只解码音频或视频，未选择的流直接跳过；默认仍然读取两路。
  int Open(const std::string &url, bool audioEnabled = true, bool videoEnabled = true);
  void Close();
  int GetAudioStreamIndex() const {
    return m_audio_index;
  }
  int GetVideoStreamIndex() const {
    return m_video_index;
  }
  double GetDurationSeconds() const {
    return m_demuxer.GetDurationSeconds();
  }
  int Seek(double seconds);

  // 阻塞读到结尾并分别排空解码器。0=全部成功，负数=FFmpeg 错误码。
  // 每次 Open 之后只能调用一次；再次读取需重新 Open。
  // 回调可为空（仍解码，只不交付输出）；回调不得重入 Open/Close/ReadAll。
  // 回调抛出异常会向外传播，本地帧/包自动释放；再次使用需重新 Open。
  // cancelled 可选，在读取包和接收帧之前检查（包含 EOF 排空阶段）。
  // 观察到取消返回 AVERROR(ECANCELED)；当前回调/阻塞 I/O 不会被强制中断。
  // 标志须存活至本次调用结束；取消后应等本次调用返回，再重置标志。
  int ReadAll(const FrameCallback &onAudio, const FrameCallback &onVideo,
              const std::atomic<bool> *cancelled = nullptr);

private:
  Demuxer m_demuxer;
  AudioDecoder m_audio_decoder;
  VideoDecoder m_video_decoder;
  int m_audio_index = -1;
  int m_video_index = -1;
  bool m_opened = false;
  bool m_read_started = false;
};
} // namespace PLAYER
