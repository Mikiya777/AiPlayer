#pragma once

#include <functional>
#include <memory>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
}

namespace PLAYER {
// FFmpeg 对象必须用配套 API 释放，不能用普通 delete。
// unique_ptr 保证中途返回或回调抛出异常时，本地包/帧也会释放。
struct PacketDeleter {
  void operator()(AVPacket *packet) const {
    av_packet_free(&packet);
  }
};
struct FrameDeleter {
  void operator()(AVFrame *frame) const {
    av_frame_free(&frame);
  }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// 回调在 ReadAll() 的调用线程中执行。frame 仅在回调期间有效。
// 后续异步队列若需保留帧，必须 av_frame_clone()/av_frame_ref() 建立自己的引用。
// timeBase 是帧 PTS 的单位；有效 PTS 转秒：frame.pts * av_q2d(timeBase)。
// 时间戳可能为 AV_NOPTS_VALUE，且不可分别把音视频首帧强行归零。
using FrameCallback = std::function<void(const AVFrame &frame, AVRational timeBase)>;
} // namespace PLAYER
