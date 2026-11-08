#pragma once
#include "writer/Muxer.h"
#include <deque>
extern "C" {
#include <libswscale/swscale.h>
}
namespace PLAYER {
class VideoEncoder {
public:
  VideoEncoder() = default;
  VideoEncoder(const VideoEncoder &) = delete;
  VideoEncoder &operator=(const VideoEncoder &) = delete;
  ~VideoEncoder();
  void Open(Muxer &muxer, int width, int height, const std::string &codecName);
  void Send(const AVFrame *rgba, int64_t pts = 0, int64_t duration = 3000);

private:
  int OpenCodec(int width, int height, const std::string &codecName);
  Muxer *m_muxer = nullptr;
  AVCodecContext *m_context = nullptr;
  SwsContext *m_scale = nullptr;
  FramePtr m_frame;
  bool m_use_libyuv = false;
  bool m_videotoolbox = false;
  struct PendingFrame {
    int64_t pts;
    int64_t duration;
    double submitted;
  };
  std::deque<PendingFrame> m_pending;
  int64_t m_last_input_pts = AV_NOPTS_VALUE;
  int m_stream = -1;
};
} // namespace PLAYER
