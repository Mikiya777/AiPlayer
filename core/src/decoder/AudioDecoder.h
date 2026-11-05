#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace PLAYER {
// 解码输出为音频采样（可能是 fltp 等格式，保留原采样率和声道布局）。
// 每条流使用自己的上下文；同一实例不能并发调用。
class AudioDecoder {
public:
  AudioDecoder() = default;
  ~AudioDecoder();
  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  // 复制流参数，初始化解码器；不持有 AVStream 的借用指针。
  int Open(const AVStream &stream);
  void Close();
  AVRational GetTimeBase() const {
    return m_time_base;
  }

  // 返回 0 表示已接受输入；EAGAIN 时先取帧，再重送同一个包。
  // nullptr 是输入结束信号：成功送入后持续 receive 直到 EOF。
  // 不接管 packet 对象；送入成功后调用者可以 unref 自己的包。
  int SendPacket(const AVPacket *packet);

  // 每次成功填充一帧；EAGAIN 表示需要更多输入；EOF 表示排空完成。
  // frame 由调用者分配；处理完后 unref，最后 free。
  int ReceiveFrame(AVFrame *frame);
  void Flush();

private:
  AVCodecContext *m_context = nullptr;
  AVRational m_time_base{0, 1};
};
} // namespace PLAYER
