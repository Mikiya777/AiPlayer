#pragma once
#include "audio/AudioPipeline.h"
extern "C" {
#include <libavfilter/avfilter.h>
}
namespace PLAYER {
// swr 后的保音调变速。线程内独占；Send 后反复 Receive，EOF 前 Send(nullptr)。
// 输出 PTS 是设备采样时间，保留首帧起点；Player 映射回原媒体时间。
class AudioTempo {
public:
  AudioTempo() = default;
  ~AudioTempo();
  AudioTempo(const AudioTempo &) = delete;
  AudioTempo &operator=(const AudioTempo &) = delete;
  int Open(const AudioOutputConfig &config, double rate);
  int Send(const AVFrame *input);
  int Receive(FramePtr &output);
  void Close();

private:
  AVFilterGraph *m_graph = nullptr;
  AVFilterContext *m_source = nullptr;
  AVFilterContext *m_sink = nullptr;
  int64_t m_next_pts = 0;
  bool m_has_pts = false;
};
} // namespace PLAYER
